#ifndef SCALLOC_STACK_INL_H_
#define SCALLOC_STACK_INL_H_

#include <stdint.h>
#include <stdio.h>

#include "atomic.h"
#include "common.h"

// Treiber stack
//
// This stack assumes that memory pushed to it is not in use anymore and that
// the memory is kPageSize aligned.
class Stack {
 public:
  static void InitModule() {} 

  void Init();
  void Push(void* p);
  void* Pop();

  void Put(void* p);
  void* Get();

 private:
  TaggedAtomic<void*, uint64_t> top_;
};

always_inline void Stack::Put(void* p) {
  Push(p);
}

always_inline void* Stack::Get() {
  return Pop();
}

always_inline void Stack::Init() {
  top_.Pack(NULL, 0);
}

always_inline void Stack::Push(void* p) {
  TaggedAtomic<void*, uint64_t> top_old;
  TaggedAtomic<void*, uint64_t> top_new;
  do {
    top_old.CopyFrom(top_);
    // write the old top's pointer into the current block
    *(reinterpret_cast<void**>(p)) = reinterpret_cast<void*>(top_old.Atomic());
    top_new.WeakPack(p, top_old.Tag() + 1);
  } while (!top_.AtomicExchange(top_old, top_new));
}

always_inline void* Stack::Pop() {
  TaggedAtomic<void*, uint64_t> top_old;
  TaggedAtomic<void*, uint64_t> top_new;
  do {
    top_old.CopyFrom(top_);
    // check whether we top points to NULL, which indicates empty
    if (top_old.Atomic() == NULL) {
      return NULL;
    }
    top_new.WeakPack(*(reinterpret_cast<void**>(top_old.Atomic())), top_old.Tag() + 1);
  } while (!top_.AtomicExchange(top_old, top_new));
  return top_old.Atomic();
}

#endif  // SCALLOC_STACK_INL_H_