// Copyright (c) 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This is a simplistic insertion-ordered map.  It behaves similarly to an STL
// map, but only implements a small subset of the map's methods.  Internally, we
// just keep a map and a list going in parallel.
//
// This class provides no thread safety guarantees, beyond what you would
// normally see with std::list.
//
// Iterators point into the list and should be stable in the face of
// mutations, except for an iterator pointing to an element that was just
// deleted.

#ifndef QUICHE_COMMON_QUICHE_LINKED_HASH_MAP_H_
#define QUICHE_COMMON_QUICHE_LINKED_HASH_MAP_H_

#include <functional>
#include <list>
#include <tuple>
#include <type_traits>
#include <utility>

//#include "absl/container/flat_hash_map.h"
#include "absl/hash/hash.h"
//#include "base/containers/hash_table.hpp"
#include "quiche/common/small_unordered_flat_map.hpp"
#include "quiche/common/platform/api/quiche_export.h"
#include "quiche/common/platform/api/quiche_logging.h"
//#include "quiche/common/bitmap_allocator.h"

namespace quiche {

// This holds a list of pair<Key, Value> items.  This list is what gets
// traversed, and it's iterators from this list that we return from
// begin/end/find.
//
// We also keep a set<list::iterator> for find.  Since std::list is a
// doubly-linked list, the iterators should remain stable.

// QUICHE_NO_EXPORT comments suppress erroneous presubmit failures.
template <class Key,                      // QUICHE_NO_EXPORT
          class Value,                    // QUICHE_NO_EXPORT
          class Hash = absl::Hash<Key>,   // QUICHE_NO_EXPORT
          class Eq = std::equal_to<Key>>  // QUICHE_NO_EXPORT
class QuicheLinkedHashMap {               // QUICHE_NO_EXPORT
 private:
  typedef std::list<std::pair<Key, Value> /*, stm::allocator<std::pair<Key, Value>>*/> ListType;

//  typedef absl::flat_hash_map<Key, typename ListType::iterator, Hash, Eq> MapType;
//  using MapType = emhash5::HashMap<Key, typename ListType::iterator, Hash, Eq>;
//  using MapType = sfl::small_unordered_flat_map<Key, typename ListType::iterator, 4, Eq>;

 public:
  typedef typename ListType::iterator iterator;
//  typedef typename ListType::reverse_iterator reverse_iterator;
  typedef typename ListType::const_iterator const_iterator;
//  typedef typename ListType::const_reverse_iterator const_reverse_iterator;
//  typedef typename MapType::key_type key_type;
  typedef typename ListType::value_type value_type;
  typedef typename ListType::size_type size_type;

  QuicheLinkedHashMap() = default;
  explicit QuicheLinkedHashMap(size_type bucket_count) {}

  QuicheLinkedHashMap(const QuicheLinkedHashMap& other) = delete;
  QuicheLinkedHashMap& operator=(const QuicheLinkedHashMap& other) = delete;
  QuicheLinkedHashMap(QuicheLinkedHashMap&& other) = default;
  QuicheLinkedHashMap& operator=(QuicheLinkedHashMap&& other) = default;

  // Returns an iterator to the first (insertion-ordered) element.  Like a map,
  // this can be dereferenced to a pair<Key, Value>.
  iterator begin() { return list_.begin(); }
  const_iterator begin() const { return list_.begin(); }

  // Returns an iterator beyond the last element.
  iterator end() { return list_.end(); }
  const_iterator end() const { return list_.end(); }

  // Returns an iterator to the last (insertion-ordered) element.  Like a map,
  // this can be dereferenced to a pair<Key, Value>.
//  reverse_iterator rbegin() { return list_.rbegin(); }
//  const_reverse_iterator rbegin() const { return list_.rbegin(); }

  // Returns an iterator beyond the first element.
//  reverse_iterator rend() { return list_.rend(); }
//  const_reverse_iterator rend() const { return list_.rend(); }

  // Front and back accessors common to many stl containers.

  // Returns the earliest-inserted element
  const value_type& front() const { return list_.front(); }

  // Returns the earliest-inserted element.
  value_type& front() { return list_.front(); }

  // Returns the most-recently-inserted element.
//  const value_type& back() const { return list_.back(); }

  // Returns the most-recently-inserted element.
  value_type& back() { return list_.back(); }

  // Clears the map of all values.
  void clear() {
    list_.clear();
  }

  // Returns true iff the map is empty.
  bool empty() const { return list_.empty(); }

  // Removes the first element from the list.
  void pop_front() { list_.erase(list_.begin()); }

  // Erases values with the provided key.  Returns the number of elements
  // erased.  In this implementation, this will be 0 or 1.
  size_type erase(const Key& key) {
    auto found = find(key);
    if (found == list_.end()) {
      return 0;
    }

    list_.erase(found);
    return 1;
  }

  // Erases the item that 'position' points to. Returns an iterator that points
  // to the item that comes immediately after the deleted item in the list, or
  // end().
  // If the provided iterator is invalid or there is inconsistency between the
  // map and list, a QUICHE_CHECK() error will occur.
  iterator erase(iterator position) {
    return list_.erase(position);
  }

  // Erases all the items in the range [first, last).  Returns an iterator that
  // points to the item that comes immediately after the last deleted item in
  // the list, or end().
  iterator erase(iterator first, iterator last) {
    while (first != last && first != end()) {
      first = erase(first);
    }
    return first;
  }

  // Finds the element with the given key.  Returns an iterator to the
  // value found, or to end() if the value was not found.  Like a map, this
  // iterator points to a pair<Key, Value>.
  iterator find(const Key& key) {
    for (auto it = list_.begin(); it != list_.end(); it++)
      if (key == it->first) return it;
    return list_.end();
  }

  const_iterator find(const Key& key) const {
    for (auto it = list_.begin(); it != list_.end(); it++)
      if (key == it->first) return it;
    return list_.end();
  }

  bool contains(const Key& key) const {
    return list_.size() && list_.end() != find(key);
  }

  // Inserts an element into the map
  bool insert(const Key& key, const Value& value) {
    // First make sure the map doesn't have a key with this value.  If it does,
    // return a pair with an iterator to it, and false indicating that we
    // didn't insert anything.
    if (list_.size() && find(key) != list_.end()) {
      return false;
    }

    // Otherwise, insert into the list first.
    list_.emplace_back(key, value);
    return true;
  }

  // Derive size_ from map_, as list::size might be O(N).
  size_type size() const { return list_.size(); }
#if 1
  std::pair<iterator, bool> emplace(const Key& key, Value&& value) {
    if (list_.size() && find(key) != list_.end()) {
      return { list_.end(), false };
    }

    // Otherwise, insert into the list first.
    list_.emplace_back(key, std::move(value));
    return {--list_.end(), true };
  }

  void swap(QuicheLinkedHashMap& other) {
    list_.swap(other.list_);
  }
#endif
 private:


  // The map component, used for speedy lookups
  //MapType map_;

  // The list component, used for maintaining insertion order
  ListType list_;
};

}  // namespace quiche

#endif  // QUICHE_COMMON_QUICHE_LINKED_HASH_MAP_H_
