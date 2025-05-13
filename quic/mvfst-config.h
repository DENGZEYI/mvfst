/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <folly/container/heap_vector_types.h>
#include <folly/small_vector.h>

namespace quic {
template <class... Args>
struct UnorderedMap : folly::F14FastMap<Args...> {};

template <class... Args>
struct ValueMap : folly::F14ValueMap<Args...> {};

template <class... Args>
struct UnorderedSet : folly::F14FastSet<Args...> {};

template <class T, std::size_t N, class... Policy>
using SmallVec = folly::small_vector<T, N, Policy...>;

template <class T, size_t N>
using InlineMapVec = SmallVec<T, N>;

template <
    typename Key,
    typename Value,
    size_t N,
    class Container = InlineMapVec<std::pair<Key, Value>, N>,
    typename = std::enable_if_t<std::is_integral<Key>::value>>
using InlineMap = folly::heap_vector_map<
    Key,
    Value,
    std::less<Key>,
    typename Container::allocator_type,
    void,
    Container>;

template <class T, size_t N>
using InlineSetVec = SmallVec<T, N>;

template <
    typename Value,
    size_t N,
    class Container = InlineSetVec<Value, N>,
    typename = std::enable_if_t<std::is_integral<Value>::value>>
using InlineSet = folly::heap_vector_set<
    Value,
    std::less<Value>,
    typename Container::allocator_type,
    void,
    Container>;

} // namespace quic
