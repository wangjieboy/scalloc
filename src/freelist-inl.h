// Copyright (c) 2013, the scalloc Project Authors.  All rights reserved.
// Please see the AUTHORS file for details.  Use of this source code is governed
// by a BSD license that can be found in the LICENSE file.

#ifndef SCALLOC_FREELIST_H_
#define SCALLOC_FREELIST_H_

#include "assert.h"
#include "common.h"

/// An unlocked free list.
class Freelist {
 public:
  inline Freelist() {}
  void InitRange(const void* start, const size_t size, size_t len);

  void Push(void* p);
  void* Pop();
  size_t Utilization();
  inline bool Empty() { return list_ == NULL; }
  inline bool Full() { return len_ == cap_; }
  inline size_t Size() { return len_; }

 private:
  size_t cap_;
  size_t len_;
  void* list_;

#ifdef DEBUG
  uintptr_t lower_;
  uintptr_t upper_;
#endif  // DEBUG

  DISALLOW_ALLOCATION();
  DISALLOW_COPY_AND_ASSIGN(Freelist);
};


inline void Freelist::InitRange(const void* start,
                                const size_t size,
                                size_t len) {
  len_ = len;
  cap_ = len;
  list_ = NULL;
  uintptr_t start_ptr = reinterpret_cast<uintptr_t>(start);
#ifdef DEBUG
  lower_ = start_ptr;
  upper_ = start_ptr + size *len;
#endif  // DEBUG
  for (; len > 0; len--) {
    // Inlined push.
    *(reinterpret_cast<void**>(start_ptr)) = list_;
    list_ = reinterpret_cast<void*>(start_ptr);
    start_ptr += size;
  }
}


inline void Freelist::Push(void* p) {
#ifdef DEBUG
  ScallocAssert((reinterpret_cast<uintptr_t>(p) >= lower_) &&
                (reinterpret_cast<uintptr_t>(p) < upper_));
#endif  // DEBUG
  *(reinterpret_cast<void**>(p)) = list_;
  list_ = p;
  len_++;
}


inline void* Freelist::Pop() {
  void* result = list_;
  if (result != NULL) {
#ifdef DEBUG
    ScallocAssert((reinterpret_cast<uintptr_t>(result) >= lower_) &&
                  (reinterpret_cast<uintptr_t>(result) < upper_));
#endif  // DEBUG
    list_ = *(reinterpret_cast<void**>(list_));
    len_--;
  }
  return result;
}


inline size_t Freelist::Utilization() {
  return 100 - ((len_ * 100) / cap_);
}

#endif  // SCALLOC_FREELIST_H_