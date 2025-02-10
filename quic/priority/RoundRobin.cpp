/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <quic/priority/RoundRobin.h>

namespace {
static constexpr size_t kBuildIndexThreshold = 30;
static constexpr size_t kDestroyIndexThreshold = 10;
} // namespace

namespace quic {

void RoundRobin::advanceAfterNext(size_t n) {
  if (advanceType_ == AdvanceType::Bytes) {
    current_ = 0;
  }
  advanceType_ = AdvanceType::Nexts;
  advanceAfter_ = n;
}

void RoundRobin::advanceAfterBytes(uint64_t bytes) {
  if (advanceType_ == AdvanceType::Nexts) {
    current_ = 0;
  }
  advanceType_ = AdvanceType::Bytes;
  advanceAfter_ = bytes;
}

bool RoundRobin::empty() const {
  return list_.empty();
}

// The caller needs to verify it never inserts a duplicate
void RoundRobin::insert(quic::PriorityQueue::Identifier value) {
  DCHECK(!erase(value)) << "Duplicate value";
  // Insert new integer at the tail of the list
  if (!useIndexMap_ && list_.size() >= kBuildIndexThreshold) {
    useIndexMap_ = true;
    buildIndex();
  }
  auto insertIt = list_.insert(nextIt_, value);
  if (list_.size() == 1) {
    nextIt_ = list_.begin();
  }
  if (useIndexMap_) {
    indexMap_[value] = insertIt;
  }
}

bool RoundRobin::erase(quic::PriorityQueue::Identifier value) {
  if (list_.empty()) {
    return false;
  }
  if (useIndexMap_) {
    auto it = indexMap_.find(value);
    if (it == indexMap_.end()) {
      return false;
    }
    auto listIt = it->second;
    indexMap_.erase(it);
    erase(listIt);
    return true;
  } else {
    // the most likely erase is from next or next - 1
    if (*nextIt_ == value) {
      erase(nextIt_);
      current_ = 0;
      return true;
    }

    // Search backwards from nextIt_ - 1 to the beginning
    auto reverseIt = std::make_reverse_iterator(nextIt_);
    auto rpos = std::find(reverseIt, list_.rend(), value);
    if (rpos != list_.rend()) {
      erase(std::prev(rpos.base()));
      return true;
    }
    // Search forwards from nextIt_ + 1 to the end
    auto pos = std::find(std::next(nextIt_), list_.end(), value);
    if (pos != list_.end()) {
      erase(pos);
      return true;
    }
    return false;
  }
}

quic::PriorityQueue::Identifier RoundRobin::getNext(
    const quic::Optional<uint64_t>& bytes) {
  CHECK(!list_.empty());
  auto ret = *nextIt_;
  consume(bytes);
  return ret;
}

[[nodiscard]] quic::PriorityQueue::Identifier RoundRobin::peekNext() const {
  CHECK(!list_.empty());
  return *nextIt_;
}

void RoundRobin::consume(const quic::Optional<uint64_t>& bytes) {
  if (advanceType_ == AdvanceType::Bytes) {
    current_ += bytes.value_or(0);
  } else {
    current_++;
  }
  maybeAdvance();
}

void RoundRobin::clear() {
  list_.clear();
  if (useIndexMap_) {
    indexMap_.clear();
    useIndexMap_ = false;
  }
  nextIt_ = list_.end();
  current_ = 0;
}

void RoundRobin::erase(ListType::iterator eraseIt) {
  if (eraseIt == nextIt_) {
    nextIt_ = list_.erase(eraseIt);
    if (nextIt_ == list_.end()) {
      nextIt_ = list_.begin();
    }
    current_ = 0;
  } else {
    list_.erase(eraseIt);
  }
  if (list_.size() < kDestroyIndexThreshold) {
    useIndexMap_ = false;
    indexMap_.clear();
  }
}

void RoundRobin::maybeAdvance() {
  CHECK(!list_.empty());
  if (current_ >= advanceAfter_) {
    ++nextIt_;
    current_ = 0;
    if (nextIt_ == list_.end()) {
      nextIt_ = list_.begin();
    }
  }
}

void RoundRobin::buildIndex() {
  for (auto it = list_.begin(); it != list_.end(); ++it) {
    indexMap_[*it] = it;
  }
}

} // namespace quic
