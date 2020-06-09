// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_ALLOCATOR_PARTITION_ALLOCATOR_ADDRESS_POOL_MANAGER_H_
#define BASE_ALLOCATOR_PARTITION_ALLOCATOR_ADDRESS_POOL_MANAGER_H_

#include <bitset>
#include <memory>

#include "base/allocator/partition_allocator/partition_alloc_constants.h"
#include "base/atomicops.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "build/build_config.h"

namespace base {

namespace internal {

using pool_handle = unsigned;

// The address space reservation is supported only on 64-bit architecture.
#if defined(ARCH_CPU_64_BITS)

// AddressPoolManager takes a reserved virtual address space and manages the
// address space allocation.
// AddressPoolManager supports up to 2 pools. One pool manages one contiguous
// reserved address space. Alloc() takes the pool handle and returns
// address regions from the specified pool. Free() also takes the pool handle
// and returns the address region back to the manager.
class BASE_EXPORT AddressPoolManager {
 public:
  static AddressPoolManager* GetInstance();

  pool_handle Add(uintptr_t address, size_t length);
  void Remove(pool_handle handle);
  char* Alloc(pool_handle handle, size_t length);
  void Free(pool_handle handle, void* ptr, size_t length);
  void ResetForTesting();

 private:
  AddressPoolManager();
  ~AddressPoolManager();

  pool_handle AllocHandle();

  class Pool {
   public:
    Pool(uintptr_t ptr, size_t length);
    ~Pool();

    uintptr_t FindChunk(size_t size);
    void FreeChunk(uintptr_t address, size_t size);

   private:
    // The bitset stores an allocation state of the address pool. 1 bit per
    // super-page: 1 = allocated, 0 = free.
    static constexpr size_t kGigaBytes = 1024 * 1024 * 1024;
    static constexpr size_t kMaxSupportedSize = 16 * kGigaBytes;
    static constexpr size_t kMaxBits = kMaxSupportedSize / kSuperPageSize;
    base::Lock lock_;
    std::bitset<kMaxBits> alloc_bitset_ GUARDED_BY(lock_);

    const size_t total_bits_;
    const uintptr_t address_begin_;
#if DCHECK_IS_ON()
    const uintptr_t address_end_;
#endif

    // A number of a bit before which we know for sure there all 1s. This is
    // a best effort hint in the sense that there still may be lots of 1s after
    // this bit, but at least we know there is no point in starting the search
    // before it.
    size_t bit_hint_;

    DISALLOW_COPY_AND_ASSIGN(Pool);
  };

  static constexpr size_t kNumPools = 2;
  std::unique_ptr<Pool> pools_[kNumPools];

  friend class NoDestructor<AddressPoolManager>;
  DISALLOW_COPY_AND_ASSIGN(AddressPoolManager);
};

#endif  // defined(ARCH_CPU_64_BITS)

}  // namespace internal
}  // namespace base

#endif  // BASE_ALLOCATOR_PARTITION_ALLOCATOR_ADDRESS_POOL_MANAGER_H_
