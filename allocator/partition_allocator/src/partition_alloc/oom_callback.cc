// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/allocator/partition_allocator/src/partition_alloc/oom_callback.h"

#include "base/allocator/partition_allocator/src/partition_alloc/partition_alloc_check.h"

namespace partition_alloc {

namespace {
PartitionAllocOomCallback g_oom_callback;
}  // namespace

void SetPartitionAllocOomCallback(PartitionAllocOomCallback callback) {
  PA_DCHECK(!g_oom_callback);
  g_oom_callback = callback;
}

namespace internal {
void RunPartitionAllocOomCallback() {
  if (g_oom_callback) {
    g_oom_callback();
  }
}
}  // namespace internal

}  // namespace partition_alloc
