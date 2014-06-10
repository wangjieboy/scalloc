// Copyright (c) 2014, the scalloc Project Authors.  All rights reserved.
// Please see the AUTHORS file for details.  Use of this source code is governed
// by a BSD license that can be found in the LICENSE file.

#ifndef SCALLOC_PROFILER_H_
#define SCALLOC_PROFILER_H_

#ifndef PROFILER

#define PROFILER_DECL
#define PROFILER_GETTER
#define PROFILER_DEALLOC(sc,type)
#define PROFILER_ALLOC(sc)
#define PROFILER_SPANPOOL_PUT(sc)
#define PROFILER_SPANPOOL_GET(sc)
#define PROFILER_BLOCKPOOL_PUT(sc)
#define PROFILER_BLOCKPOOL_GET(sc)
#define PROFILER_BLOCKPOOL_EMPTY_GET(sc)
#define PROFILER_STEAL()
#define PROFILER_NO_CLEANUP(sc)

#else  // PROFILER

#include <stdio.h>

#include "common.h"
#include "log.h"

#ifdef POLICY_CORE_LOCAL
#define PROFILER_ scalloc::CoreBuffer::GetBuffer().Profiler()
#endif  // POLICY_CORE_LOCAL
#ifdef POLICY_THREAD_LOCAL
#define PROFILER_ scalloc::ThreadCache::GetCache().Profiler()
#endif  // POLICY_THREAD_LOCAL

#define PROFILER_DECL scalloc::Profiler profiler_
#define PROFILER_GETTER \
  inline scalloc::Profiler& Profiler() { return profiler_; }

#define PROFILER_ALLOC(sc) PROFILER_.Alloc(sc)
#define PROFILER_DEALLOC(sc, type) PROFILER_.Dealloc(sc,type)
#define PROFILER_SPANPOOL_PUT(sc) PROFILER_.SpanPoolPut(sc)
#define PROFILER_SPANPOOL_GET(sc) PROFILER_.SpanPoolGet(sc)
#define PROFILER_BLOCKPOOL_PUT(sc) PROFILER_.BlockPoolPut()
#define PROFILER_BLOCKPOOL_GET(sc) PROFILER_.BlockPoolGet()
#define PROFILER_BLOCKPOOL_EMPTY_GET(sc) PROFILER_.BlockPoolEmptyGet()
#define PROFILER_STEAL() PROFILER_.Steal()
#define PROFILER_NO_CLEANUP(sc) PROFILER_.NoCleanup()

namespace scalloc {

class Profiler;
extern Profiler GlobalProfiler;

class Profiler {
 public:
  static const uint64_t kMaxChildren = 32000;

  inline void Init(Profiler* parent);
  inline void Report();
  inline void Alloc(uint64_t sc);
  inline void Dealloc(uint64_t sc, int type);
  inline void SpanPoolPut(uint64_t sc);
  inline void SpanPoolGet(uint64_t sc);
  inline void BlockPoolPut();
  inline void BlockPoolGet();
  inline void BlockPoolEmptyGet();
  inline void NoCleanup();
  inline void Steal();
  inline void Print();

 private:
  inline void Reset();
  inline void Update(Profiler* other);
  inline void FillSizeClasses(char* buffer, size_t len);

  Profiler* parent_;
  uint64_t updates_;
  uint64_t block_pool_put_;
  uint64_t block_pool_get_;
  uint64_t block_pool_empty_get_;
  uint64_t steal_;
  uint64_t no_cleanup_;
  uint64_t allocations_[kNumClasses];
  uint64_t hot_deallocations_[kNumClasses];
  uint64_t cool_deallocations_[kNumClasses];
  uint64_t slow_deallocations_[kNumClasses];
  uint64_t span_pool_get_[kNumClasses];
  uint64_t span_pool_put_[kNumClasses];
};


void Profiler::Init(scalloc::Profiler* parent) {
  if (parent != NULL) {
    LOG_CAT("profiler", kTrace, "setting parent profiler %p", parent);
    parent_ = parent;
  }
  parent_ = parent;
  Reset();
}


void Profiler::Reset() {
  for (size_t i = 0; i < kNumClasses; i++) {
    allocations_[i] = 0;
    hot_deallocations_[i] = 0;
    cool_deallocations_[i] = 0;
    slow_deallocations_[i] = 0;
    span_pool_get_[i] = 0;
    span_pool_put_[i] = 0;
  }
  block_pool_put_ = 0;
  block_pool_get_ = 0;
  block_pool_empty_get_ = 0;
  steal_ = 0;
  no_cleanup_ = 0;
  updates_ = 0;
}


void Profiler::Update(Profiler* other) {
  LOG_CAT("profiler", kTrace, "updating profiler data");
  updates_++;
  for (size_t i = 0; i < kNumClasses; i++) {
    allocations_[i] += other->allocations_[i];
    hot_deallocations_[i] += other->hot_deallocations_[i];
    cool_deallocations_[i] += other->cool_deallocations_[i];
    slow_deallocations_[i] += other->slow_deallocations_[i];
    span_pool_get_[i] += other->span_pool_get_[i];
    span_pool_put_[i] += other->span_pool_put_[i];
  }
  block_pool_put_ += other->block_pool_put_;
  block_pool_get_ += other->block_pool_get_;
  block_pool_empty_get_ += other->block_pool_empty_get_;
  steal_ += other->steal_;
  no_cleanup_ += other->no_cleanup_;
}


void Profiler::Report() {
  LOG_CAT("profiler", kTrace, "reporting profiler data");
  if (parent_ != NULL) {
    parent_->Update(this);
  }
}


void Profiler::Alloc(uint64_t sc) {
  allocations_[sc]++;
}


void Profiler::Dealloc(uint64_t sc, int type) {
  switch(type) {
    case 0:
      hot_deallocations_[sc]++;
      break;
    case 1:
      cool_deallocations_[sc]++;
      break;
    case 2:
      slow_deallocations_[sc]++;
      break;
  }
}


void Profiler::SpanPoolPut(uint64_t sc) {
  span_pool_put_[sc]++;
}


void Profiler::NoCleanup() {
  no_cleanup_++;
}

void Profiler::SpanPoolGet(uint64_t sc) {
  span_pool_get_[sc]++;
}


void Profiler::BlockPoolPut() {
  block_pool_put_++;
}


void Profiler::BlockPoolGet() {
  block_pool_get_++;
}


void Profiler::BlockPoolEmptyGet() {
  block_pool_empty_get_++;
}


void Profiler::Steal() {
  steal_++;
}


void Profiler::FillSizeClasses(char* buffer, size_t len) {
  size_t start = 0;
  for (size_t i = 0; i < kNumClasses; i++) {
    start += sprintf(
        &buffer[start],
        "    \"%lu\": { "
        "\"size\": %lu, "
        "\"objects\": %lu, "
        "\"realspan size\": %lu "
        "}%c\n",
        i,
        ClassToSize[i],
        ClassToObjects[i],
        ClassToSpanSize[i],
        (i ==  (kNumClasses - 1)) ? ' ' : ',' );
  }
}


void Profiler::Print() {
  uint64_t overall_span_pool_get = 0;
  uint64_t overall_span_pool_put = 0;
  const size_t buffer_length = 1024 * 4;
  char size_class_buffer[buffer_length] = {0};
  size_t start = 0;
  for (size_t i = 0; i < kNumClasses; i++) {
    start += sprintf(
        &size_class_buffer[start],
        "    \"%lu\": { "
        "\"allocations\": %lu, "
        "\"SP.get\": %lu, "
        "\"SP.put\": %lu, "
        "\"hot deallocations\": %lu, "
        "\"cool deallocatons\": %lu, "
        "\"slow deallocations\": %lu "
        "}%c\n",
        i,
        allocations_[i],
        span_pool_get_[i],
        span_pool_put_[i],
        hot_deallocations_[i],
        cool_deallocations_[i],
        slow_deallocations_[i],
        (i ==  (kNumClasses - 1)) ? ' ' : ',');
    ScallocAssert(start < buffer_length);
    overall_span_pool_get += span_pool_get_[i];
    overall_span_pool_put += span_pool_put_[i];
  }

  char size_class_info[buffer_length] = { 0 };
  FillSizeClasses(size_class_info, buffer_length);

  printf(
      "{\n"
      "  \"size_class_info\": {\n"
      "%s"
      "  },\n"
      "  \"updates\": %lu,\n"
      "  \"block_pool_put\": %lu,\n"
      "  \"block_pool_get\": %lu,\n"
      "  \"block_pool_empty_get\": %lu,\n"
      "  \"steal\": %lu,\n"
      "  \"no_cleanup\": %lu,\n"
      "  \"overall\": {\n"
      "    \"SP.get\": %lu,\n"
      "    \"SP.put\": %lu\n"
      "  },\n"
      "  \"size_class_profile\": {\n"
      "%s"
      "  }\n"
      "}\n",
      size_class_info,
      updates_,
      block_pool_put_,
      block_pool_get_,
      block_pool_empty_get_,
      steal_,
      no_cleanup_,
      overall_span_pool_get,
      overall_span_pool_put,
      size_class_buffer);
}

}  // namespace scalloc

#endif  // PROFILER

#endif  // SCALLOC_PROFILER_H_

