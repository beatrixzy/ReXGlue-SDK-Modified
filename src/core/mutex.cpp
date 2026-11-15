/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/thread/mutex.h>

#if defined(_WIN32)
#include "platform_win.h"
#endif

namespace rex::thread {

#if defined(_WIN32)
static_assert(sizeof(void*) == sizeof(SRWLOCK));

void global_mutex_type::lock() {
  uint32_t self = GetCurrentThreadId();
  if (owner_thread_.load(std::memory_order_relaxed) == self) {
    ++recursion_count_;
    return;
  }
  AcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&srwlock_));
  owner_thread_.store(self, std::memory_order_relaxed);
  recursion_count_ = 1;
}

void global_mutex_type::unlock() {
  if (--recursion_count_ == 0) {
    owner_thread_.store(0, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&srwlock_));
  }
}

bool global_mutex_type::try_lock() {
  uint32_t self = GetCurrentThreadId();
  if (owner_thread_.load(std::memory_order_relaxed) == self) {
    ++recursion_count_;
    return true;
  }
  if (!TryAcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&srwlock_))) {
    return false;
  }
  owner_thread_.store(self, std::memory_order_relaxed);
  recursion_count_ = 1;
  return true;
}
#endif

global_mutex_type& global_critical_region::mutex() {
  static global_mutex_type global_mutex;
  return global_mutex;
}

}  // namespace rex::thread
