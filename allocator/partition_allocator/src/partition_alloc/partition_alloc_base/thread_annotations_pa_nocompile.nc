// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This is a "No Compile Test" suite.
// https://dev.chromium.org/developers/testing/no-compile-tests

#include "base/allocator/partition_allocator/src/partition_alloc/partition_alloc_base/thread_annotations.h"

namespace {

class PA_LOCKABLE Lock {
 public:
  void Acquire() PA_EXCLUSIVE_LOCK_FUNCTION() {}
  void Release() PA_UNLOCK_FUNCTION() {}
};

class PA_SCOPED_LOCKABLE AutoLock {
 public:
  AutoLock(Lock& lock) PA_EXCLUSIVE_LOCK_FUNCTION(lock) : lock_(lock) {
    lock.Acquire();
  }
  ~AutoLock() PA_UNLOCK_FUNCTION() { lock_.Release(); }

 private:
  Lock& lock_;
};
class ThreadSafe {
 public:
  void BuggyIncrement();
 private:
  Lock lock_;
  int counter_ PA_GUARDED_BY(lock_);
};

#if defined(NCTEST_LOCK_WITHOUT_UNLOCK)  // [r"fatal error: mutex 'lock_' is still held at the end of function"]

void ThreadSafe::BuggyIncrement() {
  lock_.Acquire();
  ++counter_;
  // Forgot to release the lock.
}

#elif defined(NCTEST_ACCESS_WITHOUT_LOCK)  // [r"fatal error: writing variable 'counter_' requires holding mutex 'lock_' exclusively"]

void ThreadSafe::BuggyIncrement() {
  // Member access without holding the lock guarding it.
  ++counter_;
}

#elif defined(NCTEST_ACCESS_WITHOUT_SCOPED_LOCK)  // [r"fatal error: writing variable 'counter_' requires holding mutex 'lock_' exclusively"]

void ThreadSafe::BuggyIncrement() {
  {
    AutoLock auto_lock(lock_);
    // The AutoLock will go out of scope before the guarded member access.
  }
  ++counter_;
}

#elif defined(NCTEST_GUARDED_BY_WRONG_TYPE)  // [r"fatal error: 'guarded_by' attribute requires arguments whose type is annotated"]

int not_lockable;
int global_counter PA_GUARDED_BY(not_lockable);

// Defined to avoid link error.
void ThreadSafe::BuggyIncrement() { }

#endif

}  // anonymous namespace
