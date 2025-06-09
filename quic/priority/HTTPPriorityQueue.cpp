/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <quic/priority/HTTPPriorityQueue.h>

namespace {
constexpr size_t kBuildIndexThreshold = 100;
constexpr size_t kDestroyIndexThreshold = 50;
} // namespace

namespace quic {

/*implicit*/ HTTPPriorityQueue::Priority::Priority(
    const PriorityQueue::Priority& basePriority)
    : PriorityQueue::Priority(basePriority) {
  if (!isInitialized()) {
    getFields() = kDefaultPriority;
  }
}

HTTPPriorityQueue::Priority::Priority(uint8_t u, bool i, OrderId o) {
  auto& fields = getFields();
  fields.urgency = u;
  fields.incremental = i;
  fields.order = (i ? 0 : o);
  fields.paused = false;
}

PriorityQueue::PriorityLogFields HTTPPriorityQueue::toLogFields(
    const PriorityQueue::Priority& pri) const {
  // This is defined by the QLOG schema
  auto httpPri = static_cast<const HTTPPriorityQueue::Priority&>(pri);
  if (httpPri->paused) {
    return {{"paused", "true"}};
  }
  PriorityLogFields result;
  result.reserve(3);
  result.emplace_back("urgency", std::to_string(httpPri->urgency));
  result.emplace_back("incremental", httpPri->incremental ? "true" : "false");
  result.emplace_back("order", std::to_string(httpPri->order));

  return result;
}

quic::Optional<HTTPPriorityQueue::FindResult> HTTPPriorityQueue::find(
    Identifier id) const {
  auto it = indexMap_.find(id);
  if (it != indexMap_.end()) {
    return FindResult{it->second, it};
  }
  if (!useIndexMapForSequential_) {
    // linear search the heap
    for (size_t i = 0; i < heap_.size(); i++) {
      auto& elem = heap_[i];
      if (!elem.priority->incremental && elem.identifier == id) {
        return FindResult{IndexMapElem{false, i}, indexMap_.end()};
      }
    }
  }
  return std::nullopt;
}

void HTTPPriorityQueue::addIndex(Identifier id, IndexMapElem indexElem) {
  if (useIndexMapForSequential_ || indexElem.incremental) {
    indexMap_[id] = indexElem;
  }
}

void HTTPPriorityQueue::removeIndex(IndexMap::const_iterator it) {
  if (it != indexMap_.end() &&
      (useIndexMapForSequential_ || it->second.incremental)) {
    indexMap_.erase(it);
  }
}

void HTTPPriorityQueue::buildSequentialIndex() {
  for (size_t i = 0; i < heap_.size(); i++) {
    auto& elem = heap_[i];
    if (!elem.priority->incremental) {
      addIndex(elem.identifier, {false, i});
    }
  }
}

void HTTPPriorityQueue::destroySequentialIndex() {
  for (auto it = indexMap_.begin(); it != indexMap_.end();) {
    if (it->second.incremental) {
      ++it;
    } else {
      it = indexMap_.erase(it);
    }
  }
  useIndexMapForSequential_ = false;
}

void HTTPPriorityQueue::insertOrUpdate(
    Identifier id,
    PriorityQueue::Priority basePriority) {
  Priority priority(basePriority);
  auto findResult = find(id);
  if (findResult) {
    if (updateInSequential(findResult->elem, priority)) {
      return;
    } else {
      // moving in/out of a RR, just erase
      eraseImpl(id, findResult->elem);
      removeIndex(findResult->indexIt);
    }
  }
  if (!priority->paused) {
    insert(id, priority);
  }
}

void HTTPPriorityQueue::updateIfExist(
    Identifier id,
    PriorityQueue::Priority basePriority) {
  Priority priority(basePriority);
  auto findResult = find(id);
  if (!findResult) {
    return;
  }
  if (!updateInSequential(findResult->elem, priority)) {
    // moving in/out of a RR/paused, just erase
    bool wasIncremental = findResult->elem.incremental;
    eraseImpl(id, findResult->elem);
    if (priority->paused) {
      removeIndex(findResult->indexIt);
      return;
    }
    if (wasIncremental && !priority->incremental &&
        !useIndexMapForSequential_) {
      removeIndex(findResult->indexIt);
    } // else don't need removeIndex -- it will get updated
    insert(id, priority);
  }
}

void HTTPPriorityQueue::erase(Identifier id) {
  auto findResult = find(id);
  if (findResult) {
    Priority priority(0, false, 0);
    if (findResult->elem.incremental) {
      priority = Priority(findResult->elem.index, true, 0);
    } else {
      priority = heap_[findResult->elem.index].priority;
    }
    if (hasOpenTransaction_) {
      erased_.emplace_back(std::move(priority), id);
    }
    eraseImpl(id, findResult->elem);
    removeIndex(findResult->indexIt);
  }
  if (useIndexMapForSequential_ && heap_.size() < kDestroyIndexThreshold) {
    destroySequentialIndex();
  }
}

void HTTPPriorityQueue::clear() {
  heap_.clear();
  indexMap_.clear();
  useIndexMapForSequential_ = false;
  for (auto& rr : roundRobins_) {
    rr.clear();
  }
  roundRobinElements_ = 0;
  lowestRoundRobin_ = roundRobins_.size();
}

const HTTPPriorityQueue::Element* FOLLY_NULLABLE
HTTPPriorityQueue::top() const {
  uint8_t topPri = roundRobins_.size();
  const Element* topElem = nullptr;
  if (!heap_.empty()) {
    topElem = &heap_.front();
    topPri = topElem->priority->urgency;
  }
  if (lowestRoundRobin_ < topPri && !roundRobins_[lowestRoundRobin_].empty()) {
    return nullptr;
  }
  CHECK(topElem) << "Empty";
  return topElem;
}

quic::PriorityQueue::Identifier HTTPPriorityQueue::getNextScheduledID(
    quic::Optional<uint64_t> previousConsumed) {
  auto elem = top();
  if (elem) {
    return elem->identifier;
  } else {
    return roundRobins_[lowestRoundRobin_].getNext(previousConsumed);
  }
}

quic::PriorityQueue::Identifier HTTPPriorityQueue::peekNextScheduledID() const {
  auto elem = top();
  if (elem) {
    return elem->identifier;
  } else {
    return roundRobins_[lowestRoundRobin_].peekNext();
  }
}

void HTTPPriorityQueue::consume(quic::Optional<uint64_t> consumed) {
  auto elem = top();
  if (!elem) {
    roundRobins_[lowestRoundRobin_].consume(consumed);
  }
}

HTTPPriorityQueue::Priority HTTPPriorityQueue::headPriority() const {
  auto elem = top();
  if (elem) {
    return elem->priority;
  } else {
    return {lowestRoundRobin_, true};
  }
}

void HTTPPriorityQueue::heapifyUp(size_t index) {
  while (index > 0) {
    size_t parentIndex = (index - 1) / 2;
    if (heap_[parentIndex] <= heap_[index]) {
      break;
    }
    // Swap elements and update index map
    std::swap(heap_[parentIndex], heap_[index]);
    assignIndex(heap_[parentIndex], parentIndex);
    assignIndex(heap_[index], index);
    index = parentIndex;
  }
}

void HTTPPriorityQueue::heapifyDown(size_t index) {
  while (true) {
    size_t smallest = index;
    size_t leftChildIndex = 2 * index + 1;
    size_t rightChildIndex = 2 * index + 2;

    if (leftChildIndex < heap_.size() &&
        heap_[leftChildIndex] < heap_[smallest]) {
      smallest = leftChildIndex;
    }

    if (rightChildIndex < heap_.size() &&
        heap_[rightChildIndex] < heap_[smallest]) {
      smallest = rightChildIndex;
    }

    if (smallest == index) {
      break;
    }

    // Swap elements and update index map
    std::swap(heap_[smallest], heap_[index]);
    assignIndex(heap_[smallest], smallest);
    assignIndex(heap_[index], index);
    index = smallest;
  }
}

void HTTPPriorityQueue::assignIndex(Element& element, size_t index) {
  CHECK(!element.priority->incremental);
  addIndex(element.identifier, {false, index});
}

void HTTPPriorityQueue::insert(Identifier id, const Priority& priority) {
  if (!useIndexMapForSequential_ && heap_.size() >= kBuildIndexThreshold) {
    useIndexMapForSequential_ = true;
    buildSequentialIndex();
  }
  if (priority->incremental) {
    auto& rr = roundRobins_[priority->urgency];
    rr.insert(id);
    roundRobinElements_++;
    addIndex(id, {true, priority->urgency});
    if (priority->urgency < lowestRoundRobin_) {
      lowestRoundRobin_ = priority->urgency;
    }
  } else {
    heap_.emplace_back(priority, id);
    auto index = heap_.size() - 1;
    addIndex(id, {false, index});
    heapifyUp(index);
  }
}

bool HTTPPriorityQueue::updateInSequential(
    IndexMapElem indexElem,
    Priority priority) {
  if (priority->paused) {
    return false;
  }
  if (indexElem.incremental || priority->incremental) {
    if (indexElem.incremental && priority->incremental) {
      return indexElem.index == priority->urgency;
    }
    return false;
  }
  auto index = indexElem.index;
  auto& elem = heap_[index];
  if (elem.priority == priority) {
    return true; // no-op
  }
  std::swap(elem.priority, priority);
  if (elem.priority < priority) {
    heapifyUp(index);
  } else {
    heapifyDown(index);
  }
  return true;
}

void HTTPPriorityQueue::eraseImpl(Identifier id, IndexMapElem indexElem) {
  auto index = indexElem.index;
  if (indexElem.incremental) {
    auto& rr = roundRobins_[index];
    rr.erase(id);
    roundRobinElements_--;
    if (index == lowestRoundRobin_ && rr.empty()) {
      while (lowestRoundRobin_ < roundRobins_.size() &&
             roundRobins_[lowestRoundRobin_].empty()) {
        lowestRoundRobin_++;
      }
    }
  } else {
    auto lastIndex = heap_.size() - 1;
    std::swap(heap_[index], heap_[lastIndex]);
    assignIndex(heap_[index], index);
    heap_.pop_back();

    if (index != lastIndex) {
      if (index > 0 && heap_[index] < heap_[(index - 1) / 2]) {
        heapifyUp(index);
      } else {
        heapifyDown(index);
      }
    } // special case, erasing the last element
  }
}

} // namespace quic
