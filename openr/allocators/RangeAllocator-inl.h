/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef RANGE_ALLOCATOR_H_
#error This file may only be included from RangeAllocator.h
#endif

////////// Implementation details for RangeAllocator.h /////////////

namespace openr {

namespace details {

template <typename T>
std::string
primitiveToBinary(const T t) {
  return std::string(reinterpret_cast<const char*>(&t), sizeof(T));
}

template <typename T>
T
binaryToPrimitive(const std::string& s) {
  CHECK_EQ(sizeof(T), s.size());
  T t;
  memcpy(
      reinterpret_cast<void*>(&t),
      reinterpret_cast<const void*>(s.data()),
      s.size());
  return t;
}

} // namespace details

template <typename T>
RangeAllocator<T>::RangeAllocator(
    const std::string& nodeName,
    const std::string& keyPrefix,
    KvStoreClientInternal* const kvStoreClient,
    std::function<void(std::optional<T>)> callback,
    const std::chrono::milliseconds minBackoffDur /* = 50ms */,
    const std::chrono::milliseconds maxBackoffDur /* = 2s */,
    const bool overrideOwner /* = true */,
    const std::function<bool(T)> checkValueInUseCb,
    const std::chrono::milliseconds rangeAllocTtl,
    const std::string& area)
    : nodeName_(nodeName),
      keyPrefix_(keyPrefix),
      kvStoreClient_(kvStoreClient),
      eventBase_(kvStoreClient->getOpenrEventBase()),
      callback_(std::move(callback)),
      overrideOwner_(overrideOwner),
      backoff_(minBackoffDur, maxBackoffDur),
      checkValueInUseCb_(std::move(checkValueInUseCb)),
      rangeAllocTtl_(rangeAllocTtl),
      area_(area) {
  timeout_ =
      fbzmq::ZmqTimeout::make(eventBase_->getEvb(), [this]() mutable noexcept {
        CHECK(allocateValue_.has_value());
        auto allocateValue = allocateValue_.value();
        allocateValue_.reset();
        tryAllocate(allocateValue);
      });
}

template <typename T>
RangeAllocator<T>::~RangeAllocator() {
  VLOG(2) << "RangeAllocator: Destructing " << nodeName_ << ", " << keyPrefix_;
  // We need to cancel any pending timeout
  if (timeout_) {
    timeout_.reset();
    allocateValue_.reset();
  }

  // Unsubscribe from KvStoreClientInternal if we have been to
  if (myValue_) {
    const auto myKey = createKey(*myValue_);
    kvStoreClient_->unsubscribeKey(myKey);
    kvStoreClient_->unsetKey(myKey, area_);
  }
}

template <typename T>
std::string
RangeAllocator<T>::createKey(const T val) const noexcept {
  return folly::sformat("{}{}", keyPrefix_, val);
}

template <typename T>
void
RangeAllocator<T>::startAllocator(
    const std::pair<T, T> allocRange, const std::optional<T> maybeInitValue) {
  CHECK(not hasStarted_) << "Already started";
  hasStarted_ = true;

  allocRange_ = allocRange;
  CHECK_LE(allocRange_.first, allocRange_.second) << "Invalid range.";
  T initValue;
  if (maybeInitValue.has_value()) {
    initValue = maybeInitValue.value();
    // maybeInitValue may be outside of allocation range, e.g., initial dump
    // from kvstore gets invalid prefix index from previous incarnation
    if (*maybeInitValue < allocRange_.first) {
      LOG(ERROR) << "Initial value " << *maybeInitValue
                 << " is less than lower bound " << allocRange_.first
                 << ", using lower bound instead";
      initValue = allocRange_.first;
    } else if (*maybeInitValue > allocRange_.second) {
      LOG(ERROR) << "Initial value " << *maybeInitValue
                 << " is greater than upper bound " << allocRange_.second
                 << ", ussing upper bound instead";
      initValue = allocRange_.second;
    }
  } else {
    initValue = allocRange_.first;
  }
  allocRangeSize_ = allocRange_.second - allocRange_.first + 1;

  // Subscribe to changes in KvStore
  VLOG(2) << "RangeAllocator: Created. Scheduling first tryAllocate. "
          << "Node: " << nodeName_ << ", Prefix: " << keyPrefix_;
  allocateValue_ = initValue;
  timeout_->scheduleTimeout(backoff_.getTimeRemainingUntilRetry());
}

template <typename T>
bool
RangeAllocator<T>::isRangeConsumed() const {
  const auto maybeKeyMap = kvStoreClient_->dumpAllWithPrefix(keyPrefix_, area_);
  CHECK(maybeKeyMap.has_value())
      << "Failed to dump keys with prefix: " << keyPrefix_
      << " from kvstore in area: " << area_;
  T count = 0;
  for (const auto& kv : *maybeKeyMap) {
    const auto val =
        details::binaryToPrimitive<T>(kv.second.value_ref().value());
    if (val >= allocRange_.first && val <= allocRange_.second) {
      ++count;
    }
  }
  CHECK(count <= allocRangeSize_);
  return (count == allocRangeSize_);
}

template <typename T>
std::optional<T>
RangeAllocator<T>::getValueFromKvStore() const {
  const auto maybeKeyMap = kvStoreClient_->dumpAllWithPrefix(keyPrefix_, area_);
  CHECK(maybeKeyMap.has_value())
      << "Failed to dump keys with prefix: " << keyPrefix_
      << " from kvstore in area: " << area_;
  for (const auto& kv : *maybeKeyMap) {
    if (kv.second.originatorId == nodeName_) {
      const auto val =
          details::binaryToPrimitive<T>(kv.second.value_ref().value());
      CHECK_EQ(kv.first, createKey(val));
      return val;
    }
  }
  return std::nullopt;
}

template <typename T>
void
RangeAllocator<T>::tryAllocate(const T newVal) noexcept {
  // Sanity check. We should not have any previously allocated value.
  CHECK(!myValue_.has_value())
      << "We have previously allocated value " << myValue_.value();

  VLOG(1) << "RangeAllocator " << nodeName_ << ": trying to allocate "
          << newVal;

  // Check for any existing value in KvStore
  const auto newKey = createKey(newVal);
  const auto maybeThriftVal = kvStoreClient_->getKey(newKey, area_);
  if (maybeThriftVal) {
    DCHECK_EQ(1, maybeThriftVal->version);
  }

  // Check if we can own the value or not
  const bool shouldOwnOther = not maybeThriftVal or
      (overrideOwner_ && nodeName_ > maybeThriftVal->originatorId) or
      // Following condition is to prefer range alloc keys with TTL over keys
      // without TTL. Old node will never try to steal keys from new node
      // if overrideOwner is set to false
      // We are trying this only when overrideOwner_ is set to false otherwise
      // nodes whose keys are stolen will try to get back their keys as well
      (!overrideOwner_ && maybeThriftVal->ttl == Constants::kTtlInfinity);
  const bool shouldOwnMine =
      maybeThriftVal and (nodeName_ == maybeThriftVal->originatorId);

  // If we cannot own then we should try some other value
  if (!shouldOwnOther && !shouldOwnMine) {
    VLOG(1) << "RangeAllocator: failed to allocate " << newVal << " bcoz of "
            << maybeThriftVal->originatorId;
    scheduleAllocate(newVal);
    return;
  }
  // check if prefix index is already in use
  if (checkValueInUseCb_ and checkValueInUseCb_(newVal)) {
    VLOG(1) << "RangeAllocator: failed to allocate " << newVal
            << " as value already exists";
    scheduleAllocate(newVal);
    return;
  }

  if (shouldOwnOther) {
    myRequestedValue_ = newVal;
    // Either no one owns it or owner has lower originator ID
    // Set new value in KvStore
    auto ttlVersion = maybeThriftVal ? maybeThriftVal->ttlVersion + 1 : 0;
    const auto ret = kvStoreClient_->setKey(
        newKey,
        thrift::Value(
            apache::thrift::FRAGILE,
            1 /* version */,
            nodeName_ /* originatorId */,
            details::primitiveToBinary(newVal) /* value */,
            rangeAllocTtl_.count() /* ttl */,
            ttlVersion /* ttl version */,
            0 /* hash */),
        area_);
    CHECK(ret.has_value());
  } else {
    CHECK(shouldOwnMine);
    CHECK_EQ(nodeName_, maybeThriftVal->originatorId);
    // We own it: this can occur if the node reboots w/ kvstore intact
    // Let the application know of newly allocated value
    // We set back via KvStoreClientInternal so that ttl is published regularly
    auto newValue = *maybeThriftVal;
    newValue.ttlVersion += 1; // bump ttl version
    newValue.ttl = rangeAllocTtl_.count(); // reset ttl
    kvStoreClient_->setKey(newKey, newValue, area_);
    myValue_ = newVal;
    callback_(myValue_);
  }

  // Subscribe to updates of this newKey
  kvStoreClient_->subscribeKey(
      newKey,
      [this](
          const std::string& key,
          std::optional<thrift::Value> thriftVal) noexcept {
        if (thriftVal.has_value()) {
          keyValUpdated(key, thriftVal.value());
        }
      },
      false,
      area_);
}

template <typename T>
void
RangeAllocator<T>::scheduleAllocate(const T seedVal) noexcept {
  // Apply exponential backoff
  backoff_.reportError();

  // Use random value selection logic based on seedVal
  std::mt19937_64 gen(seedVal + folly::Random::rand64());
  std::uniform_int_distribution<T> dist(allocRange_.first, allocRange_.second);
  auto newVal = dist(gen);

  const auto maybeKeyMap = kvStoreClient_->dumpAllWithPrefix(keyPrefix_, area_);
  CHECK(maybeKeyMap.has_value())
      << "Failed to dump keys with prefix: " << keyPrefix_
      << " from kvstore in area: " << area_;
  const auto valOwners =
      folly::gen::from(*maybeKeyMap) |
      folly::gen::map([](std::pair<std::string, thrift::Value> const& kv) {
        return std::make_pair(
            details::binaryToPrimitive<T>(kv.second.value_ref().value()),
            kv.second.originatorId);
      }) |
      folly::gen::as<
          std::unordered_map<T /* value */, std::string /* owner */>>();

  // look for a value I can own
  T i;
  for (i = 0; i < allocRangeSize_; ++i) {
    const auto it = valOwners.find(newVal);
    // not owned yet or owned by higher originator if override is allowed
    if (it == valOwners.end() or (overrideOwner_ and nodeName_ >= it->second)) {
      if (!checkValueInUseCb_ or !checkValueInUseCb_(newVal)) {
        // found
        break;
      }
    }
    // try next
    newVal = (newVal < allocRange_.second) ? (newVal + 1) : allocRange_.first;
  }
  if (i == allocRangeSize_) {
    LOG(ERROR) << "All values are owned by higher originatorIds";
  }

  // Schedule timeout to allocate new value
  allocateValue_ = newVal;
  timeout_->scheduleTimeout(backoff_.getTimeRemainingUntilRetry());
}

template <typename T>
void
RangeAllocator<T>::keyValUpdated(
    const std::string& key, const thrift::Value& thriftVal) noexcept {
  const T val = details::binaryToPrimitive<T>(thriftVal.value_ref().value());

  // Some sanity checks
  CHECK_EQ(1, thriftVal.version);
  // no timeout being scheduled
  CHECK(!timeout_->isScheduled());
  // only subscribed to requested/allocated value change
  CHECK(myRequestedValue_ or myValue_);
  CHECK_EQ(myValue_ ? *myValue_ : *myRequestedValue_, val);

  // this occurs when I submit a key to kvstore owned by a lower id1
  // before my id or even higher id overrides it, an intermediate id2
  // (id1 < id2 < my id) overrides and triggers key update
  // just ignore it and wait for key update with my id or even higher id
  if (thriftVal.originatorId < nodeName_) {
    return;
  }

  if (nodeName_ == thriftVal.originatorId) {
    VLOG(3) << "RangeAllocator " << nodeName_ << ": Won " << val;
    // Our own advertisement got echoed back
    // Let the application know of newly allocated value
    myValue_ = val;
    callback_(myValue_);

    // Clear backoff
    backoff_.reportSuccess();
  } else {
    // We lost the currently trying value or allocated value
    VLOG(3) << "RangeAllocator " << nodeName_ << ": Lost " << val
            << " with battle against " << thriftVal.originatorId;

    // Let user know of withdrawal of key if it has been allocated before
    if (myValue_) {
      CHECK_LT(nodeName_, thriftVal.originatorId)
          << "Lost to higher originatorId";
      CHECK_EQ(*myValue_, val);
      callback_(std::nullopt);
      myValue_.reset();
    }

    // Unsubscribe to update of lost value
    kvStoreClient_->unsubscribeKey(key);
    kvStoreClient_->unsetKey(key, area_);
    // Schedule allocation for new value
    scheduleAllocate(val);
  }
}

} // namespace openr
