// Copyright (c) 2014, the scalloc Project Authors.  All rights reserved.
// Please see the AUTHORS file for details.  Use of this source code is governed
// by a BSD license that can be found in the LICENSE file.

#ifndef SCALLOC_ALLOCATORS_SMALL_ALLOCATOR_H_
#define SCALLOC_ALLOCATORS_SMALL_ALLOCATOR_H_

#include <pthread.h>
#include <stdint.h>

#include <new>

#include "allocators/arena.h"
#include "allocators/block_pool.h"
#include "assert.h"
#include "collector.h"
#include "common.h"
#include "headers.h"
#include "list-inl.h"
#include "lock_utils-inl.h"
#include "span_pool.h"
#include "size_classes.h"

#ifdef PROFILER_ON
#include "profiler.h"
#endif  // PROFILER_ON

namespace scalloc {

enum LockMode {
  kLocal = 0,
  kSizeClassLocked
};


template<LockMode MODE = kLocal>
class ScallocCore {
 public:
  static void Init(TypedAllocator<ScallocCore>* alloc);
  static void Destroy(ScallocCore* thiz);
  static inline bool Enabled() { return enabled_; }
  static ScallocCore* New(const uint64_t id);

  void* Allocate(const size_t size);
  void Free(void* p, SpanHeader* hdr);

 private:
  explicit ScallocCore(const uint64_t id);

  void AddCoolSpan(const size_t sc, SpanHeader* span);
  void RemoveCoolSpan(const size_t sc, SpanHeader* span);
  void AddSlowSpan(const size_t sc, SpanHeader* span);
  void RemoveSlowSpan(const size_t sc, SpanHeader* node);

  void* AllocateInSizeClass(const size_t sc);
  void FreeInSizeClass(const size_t sc, void* p, SpanHeader* hdr);
  void* AllocateNoSlab(const size_t sc);
  SpanHeader* InitSlab(uintptr_t block, size_t len, const size_t sc);
  void Refill(const size_t sc);
  void SetActiveSlab(const size_t sc, const SpanHeader* hdr);

  static TypedAllocator<ScallocCore<MODE>>* allocator;
  static bool enabled_;

  // Only used with LockMode::kSizeClassLocked.
  pthread_mutex_t size_class_lock_[kNumClasses];

  uint64_t id_;
  uint64_t me_active_;
  uint64_t me_inactive_;
  SpanHeader* hot_span_[kNumClasses];
  SpanHeader* cool_spans_[kNumClasses];
  ListNode* slow_spans_[kNumClasses];

  TypedAllocator<ListNode> node_allocator;

  DISALLOW_COPY_AND_ASSIGN(ScallocCore);
};


template<LockMode MODE>
TypedAllocator<ScallocCore<MODE>>* ScallocCore<MODE>::allocator;


template<LockMode MODE>
bool ScallocCore<MODE>::enabled_;


template<LockMode MODE>
inline ScallocCore<MODE>::ScallocCore(const uint64_t id) : id_(id) {
  ActiveOwner dummy;
  dummy.Reset(true, id_);
  me_active_ = dummy.raw;
  dummy.Reset(false, id_);
  me_inactive_ = dummy.raw;

  node_allocator.Init(kPageSize, 64, "node_alloc");

  for (size_t i = 0; i < kNumClasses; i++) {
    hot_span_[i] = NULL;
    cool_spans_[i] = NULL;
    slow_spans_[i] = NULL;
    pthread_mutex_init(&size_class_lock_[i], NULL);
  }
}


template<LockMode MODE>
inline void ScallocCore<MODE>::AddCoolSpan(const size_t sc,
                                              SpanHeader* span) {
  LOG(kTrace, "[ScallocCore] adding to list of cool spans %p", span);
  span->prev = NULL;
  span->next = cool_spans_[sc];
  if (cool_spans_[sc] != NULL) {
    cool_spans_[sc]->prev = span;
  }
    cool_spans_[sc] = span;
}


template<LockMode MODE>
inline void ScallocCore<MODE>::RemoveCoolSpan(const size_t sc,
                                                 SpanHeader* span) {
  LOG(kTrace, "[ScallocCore] removing from list of cool spans %p", span);
  if (span->prev != NULL) {
    reinterpret_cast<SpanHeader*>(span->prev)->next = span->next;
  }
  if (span->next != NULL) {
    reinterpret_cast<SpanHeader*>(span->next)->prev = span->prev;
  }
  if (cool_spans_[sc] == span) {
    cool_spans_[sc] = reinterpret_cast<SpanHeader*>(span->next);
  }
  span->prev = NULL;
  span->next = NULL;
}


template<LockMode MODE>
inline void ScallocCore<MODE>::AddSlowSpan(const size_t sc,
                                              SpanHeader* span) {
#ifdef REUSE_SLOW_SPANS
  ListNode* n = node_allocator.New();
  n->prev = NULL;
  n->data = reinterpret_cast<void*>(span);
  n->next = slow_spans_[sc];
  if (slow_spans_[sc] != NULL) {
    slow_spans_[sc]->prev = n;
  }
  slow_spans_[sc] = n;
#endif  // REUSE_SLOW_SPANS
}


template<LockMode MODE>
inline void ScallocCore<MODE>::RemoveSlowSpan(const size_t sc,
                                                 SpanHeader* span) {
#ifdef REUSE_SLOW_SPANS
  ListNode* n = slow_spans_[sc];
  while (n != NULL) {
    if (n->data == span) {
      if (n->prev != NULL) {
        n->prev->next = n->next;
      } else {
        slow_spans_[sc] = n->next;
      }
      if (n->next != NULL) {
        n->next->prev = n->prev;
      }
      node_allocator.Delete(n);
      return;
    }
    n = n->next;
  }
  UNREACHABLE();
#endif  // REUSE_SLOW_SPANS
}


template<LockMode MODE>
inline void ScallocCore<MODE>::SetActiveSlab(const size_t sc,
                                                const SpanHeader* hdr) {
  // Prepend current hot span to the list of cool spans.
  if (hot_span_[sc] != NULL) {
    LOG(kTrace, "{%lu} hot span -> cool span %p, utilization: %lu",
        sc, hot_span_[sc], hot_span_[sc]->Utilization());
    AddCoolSpan(sc, hot_span_[sc]);
  }

  hot_span_[sc] = const_cast<SpanHeader*>(hdr);
}


template<>
inline void* ScallocCore<LockMode::kLocal>::Allocate(const size_t size) {
  const size_t sc = SizeToClass(size);
  return AllocateInSizeClass(sc);
}


template<>
inline void* ScallocCore<LockMode::kSizeClassLocked>::Allocate(
    const size_t size) {
  const size_t sc = SizeToClass(size);
  LockScopePthread(size_class_lock_[sc]);
  return AllocateInSizeClass(sc);
}


template<LockMode MODE>
inline void* ScallocCore<MODE>::AllocateInSizeClass(const size_t sc) {
  void* result;
  SpanHeader* hdr = hot_span_[sc];

  if (UNLIKELY(hdr == NULL)) {
    return AllocateNoSlab(sc);
  }

  if ((result = hdr->flist.Pop()) != NULL) {
    LOG(kTrace, "[ScallocCore] returning object from active span."
                " utilization: %lu", hdr->Utilization());
#ifdef PROFILER_ON
    Profiler::GetProfiler().LogAllocation(size);
#endif  // PROFILER_ON
    return result;
  }

  return AllocateNoSlab(sc);
}


template<>
inline void ScallocCore<LockMode::kLocal>::Free(void* p, SpanHeader* hdr) {
  const size_t sc = hdr->size_class;
  FreeInSizeClass(sc, p, hdr);
}


template<>
inline void ScallocCore<LockMode::kSizeClassLocked>::Free(
    void* p, SpanHeader* hdr) {
  const size_t sc = hdr->size_class;
  LockScopePthread(size_class_lock_[sc]);
  FreeInSizeClass(sc, p, hdr);
}


template<LockMode MODE>
inline void ScallocCore<MODE>::FreeInSizeClass(
    const size_t sc, void* p, SpanHeader* hdr) {
  SpanHeader* const cur_sc_hdr = hot_span_[hdr->size_class];

  // p may be an address returned by posix_memalign(). We need to fix this.
  // |---SpanHeader---|---block---|---block---|---...---|
  // p may be anywhere in a block (because of memalign), hence we need to map it
  // back to its block starting address.
  const uintptr_t blocksize_fixed_p =
      reinterpret_cast<uintptr_t>(p) - hdr->flist_aligned_blocksize_offset;
  p = reinterpret_cast<void*>(
      reinterpret_cast<uintptr_t>(p) - (blocksize_fixed_p % ClassToSize[sc]));

  if (hdr->aowner.raw == me_active_) {
      // Local free for the currently used span.
#ifdef PROFILER_ON
      Profiler::GetProfiler().LogDeallocation(sc);
#endif  // PROFILER_ON
      hdr->flist.Push(p);
      LOG(kTrace, "[ScallocCore] free in active local block at %p, "
                  "block: %p, sc: %lu, utilization: %lu",
          p, hdr, sc, hdr->Utilization());
      if (hdr != cur_sc_hdr &&
          (hdr->Utilization() < kSpanReuseThreshold)) {
        if (UNLIKELY(hdr->flist.Full())) {
          RemoveCoolSpan(sc, hdr);
#ifdef MADVISE_SAME_THREAD
          SpanPool::Instance().Put(hdr, hdr->size_class, hdr->aowner.owner);
#else
          Collector::Put(hdr);
#endif  // MADVISE_SAME_THREAD
        } else {
          RemoveCoolSpan(sc, hdr);
          AddSlowSpan(sc, hdr);
          hdr->aowner.active = false;
        }
      }
      return;
  } else if (hdr->aowner.raw == me_inactive_) {
    // Local free in already globally available span.
    if (__sync_bool_compare_and_swap(
          &hdr->aowner.raw, me_inactive_, me_active_)) {
#ifdef PROFILER_ON
      Profiler::GetProfiler().LogDeallocation(sc, false);
#endif  // PROFILER_ON
      LOG(kTrace, "[ScallocCore] free in retired local block at %p, "
                  "sc: %lu", p, sc);
      hdr->flist.Push(p);

      if (cur_sc_hdr->Utilization() > kLocalReuseThreshold) {
        RemoveSlowSpan(sc, hdr);
        SetActiveSlab(sc, hdr);
#ifdef PROFILER_ON
        Profiler::GetProfiler().LogSpanReuse();
#endif  // PROFILER_ON
        return;
      }

      if (hdr->flist.Full()) {
        LOG(kTrace, "{%lu}  returning span: %p", sc, hdr);
        RemoveSlowSpan(sc, hdr);
#ifdef MADVISE_SAME_THREAD
        SpanPool::Instance().Put(hdr, hdr->size_class, hdr->aowner.owner);
#else
        Collector::Put(hdr);
#endif  // MADVISE_SAME_THREAD
        return;
      }

      MemoryBarrier();
      hdr->aowner.active = false;
      return;
    }
  }
#ifdef PROFILER_ON
  Profiler::GetProfiler().LogDeallocation(hdr->size_class, false, true);
#endif  // PROFILER_ON
  LOG(kTrace, "[ScallocCore] remote free for %p, owner: %lu, me: %lu",
      p, hdr->aowner.owner, id_);
  BlockPool::Instance().Free(p, hdr->size_class, hdr->remote_flist);
}


template<LockMode MODE>
void ScallocCore<MODE>::Init(TypedAllocator<ScallocCore>* alloc) {
  enabled_ = true;
  allocator = alloc;
}


template<LockMode MODE>
ScallocCore<MODE>* ScallocCore<MODE>::New(const uint64_t id) {
  LOG(kTrace, "[ScallocCore] New; id: %lu", id);
  return new(allocator->New()) ScallocCore<MODE>(id);
}


template<LockMode MODE>
void* ScallocCore<MODE>::AllocateNoSlab(const size_t sc) {
  // Size class 0 represents an object of size 0, which results in malloc()
  // returning NULL.
  if (sc == 0) {
    return NULL;
  }

  // if (hot_span_[sc] != NULL) {
#ifdef PROFILER_ON
    Profiler::GetProfiler().LogAllocation(size);
#endif  // PROFILER_ON
    // Only try to steal we had a span at least once.
    SpanHeader* hdr;
    void* p = BlockPool::Instance().Allocate(sc, id_, &hdr);
    if (p != NULL) {
#ifdef PROFILER_ON
      Profiler::GetProfiler().LogBlockStealing();
#endif  // PROFILER_ON
      if (hdr != NULL) {
        SetActiveSlab(sc, hdr);
#ifdef PROFILER_ON
        Profiler::GetProfiler().LogSpanReuse(true);
#endif  // PROFILER_ON
      } else {
        if ((SpanHeader::GetFromObject(p)->aowner.owner % utils::Cpus()) !=
                (id_ % utils::Cpus())) {
          Refill(sc);
        }
      }
      return p;
    }
  // }

  Refill(sc);
  return AllocateInSizeClass(sc);
}


template<LockMode MODE>
void ScallocCore<MODE>::Refill(const size_t sc) {
#ifdef PROFILER_ON
  Profiler::GetProfiler().LogSizeclassRefill();
#endif  // PROFILER_ON
  LOG(kTrace, "[ScallocCore] refilling size class: %lu, object size: %lu",
      sc, ClassToSize[sc]);

  // We are not checking cool spans, because their utilization is per
  // definition > kReuseThreshold, e.g. >80%.

  SpanHeader* span;

#ifdef REUSE_SLOW_SPANS
  // Try to reactivate a slow span.
  ListNode* tmp;
  ListNode* n = slow_spans_[sc];
  while (n != NULL) {
    tmp = n;
    n = n->next;
    // Remove it.
    slow_spans_[sc] = tmp->next;
    if (tmp->next != NULL) {
      tmp->next->prev = NULL;
    }
    span = reinterpret_cast<SpanHeader*>(tmp->data);
    ScallocAssert(span != NULL);
    node_allocator.Delete(tmp);
    // Try to get span.
    if (span->aowner.owner == id_ &&
        span->size_class == sc &&
        __sync_bool_compare_and_swap(&span->aowner.raw,
                                     me_inactive_,
                                     me_active_)) {
      SetActiveSlab(sc, span);
      return;
    }
  }
#endif  // REUSE_SLOW_SPANS

  // Get a span from SP.
  bool reusable = false;
  span = SpanPool::Instance().Get(sc, id_, &reusable);
  ScallocAssert(span != 0);
  span->Init(sc, id_, reusable);
  SetActiveSlab(sc, span);
  return;
}


template<LockMode MODE>
void ScallocCore<MODE>::Destroy(ScallocCore* thiz) {
  // Destroying basically means giving up hot and cool spans.  Remotely freed
  // blocks keep the span in the system, i.e., it is not released from the
  // allocator. This is similar to keeping a buffer of objects. Spans will
  // eventually be reused, since they are globally available, i.e., stealable.

  SpanHeader* cur;
  for (size_t i = 0; i < kNumClasses; i++) {
    // Hot spans.
    if (thiz->hot_span_[i] != NULL) {
      cur = thiz->hot_span_[i];
      if (cur->flist.Full()) {
#ifdef MADVISE_SAME_THREAD
        SpanPool::Instance().Put(cur, cur->size_class, cur->aowner.owner);
#else
        Collector::Put(cur);
#endif  // MADVISE_SAME_THREAD
      } else {
        thiz->hot_span_[i]->aowner.active = false;
      }
    }
    thiz->hot_span_[i] = NULL;

    // Cool spans.
    cur = reinterpret_cast<SpanHeader*>(thiz->cool_spans_[i]);
    SpanHeader* tmp;
    while (cur != NULL) {
      LOG(kTrace, "[ScallocCore]: making span global %p", cur);
      tmp = cur;
      cur = reinterpret_cast<SpanHeader*>(cur->next);
      MemoryBarrier();
      tmp->next = NULL;
      tmp->prev = NULL;
      tmp->aowner.active = false;
    }
    thiz->cool_spans_[i] = NULL;

#ifdef REUSE_SLOW_SPANS
    // Slow spans.
    ListNode* tmp2;
    ListNode* n = thiz->slow_spans_[i];
    while (n != NULL) {
      tmp2 = n;
      n = n->next;
      thiz->node_allocator.Delete(tmp2);
    }
    thiz->slow_spans_[i]= NULL;
#endif  // REUSE_SLOW_SPANS
  }

  ScallocCore::allocator->Delete(thiz);
}

}  // namespace scalloc


#endif  // SCALLOC_ALLOCATORS_SMALL_ALLOCATOR_H_