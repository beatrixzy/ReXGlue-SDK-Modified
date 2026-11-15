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

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace rex::thread {

#if defined(_WIN32)
// A recursive lock over an SRW lock, as Xenia uses for its global mutex on Windows.
class alignas(64) global_mutex_type {
 public:
  global_mutex_type() = default;
  global_mutex_type(const global_mutex_type&) = delete;
  global_mutex_type& operator=(const global_mutex_type&) = delete;

  void lock();
  void unlock();
  bool try_lock();

 private:
  // An SRWLOCK, kept as its single pointer-sized field so this header needs no windows.h.
  void* srwlock_ = nullptr;
  // Only the owner stores its own id here, so another thread can never read its own id
  // unless it holds the lock.
  std::atomic<uint32_t> owner_thread_{0};
  uint32_t recursion_count_ = 0;
};
#else
using global_mutex_type = std::recursive_mutex;
#endif
using global_unique_lock_type = std::unique_lock<global_mutex_type>;

// The global critical region mutex singleton.
// This must guard any operation that may suspend threads or be sensitive to
// being suspended such as global table locks and such.
// To prevent deadlocks this should be the first lock acquired and be held
// for the entire duration of the critical region (longer than any other lock).
//
// As a general rule if some code can only be accessed from the guest you can
// guard it with only the global critical region and be assured nothing else
// will touch it. If it will be accessed from non-guest threads you may need
// some additional protection.
//
// You can think of this as disabling interrupts in the guest. The thread in the
// global critical region has exclusive access to the entire system and cannot
// be preempted. This also means that all activity done while in the critical
// region must be extremely fast (no IO!), as it has the chance to block any
// other thread until its done.
//
// For example, in the following situation thread 1 will not be able to suspend
// thread 0 until it has exited its critical region, preventing it from being
// suspended while holding the table lock:
//   [thread 0]:
//     DoKernelStuff():
//       auto global_lock = global_critical_region_.Acquire();
//       std::lock_guard<std::mutex> table_lock(table_mutex_);
//       table_->InsertStuff();
//   [thread 1]:
//     MySuspendThread():
//       auto global_lock = global_critical_region_.Acquire();
//       ::SuspendThread(thread0);
//
// To use the region it's strongly recommended that you keep an instance near
// the data requiring it. This makes it clear to those reading that the data
// is protected by the global critical region. For example:
// class MyType {
//   // Implies my_list_ is protected:
//   xe::global_critical_region global_critical_region_;
//   std::list<...> my_list_;
// };
class global_critical_region {
 public:
  static global_mutex_type& mutex();

  // Acquires a lock on the global critical section.
  // Use this when keeping an instance is not possible. Otherwise, prefer
  // to keep an instance of global_critical_region near the members requiring
  // it to keep things readable.
  static global_unique_lock_type AcquireDirect() {
    return global_unique_lock_type(mutex());
  }

  // Acquires a lock on the global critical section.
  inline global_unique_lock_type Acquire() {
    return global_unique_lock_type(mutex());
  }

  // Acquires a deferred lock on the global critical section.
  inline global_unique_lock_type AcquireDeferred() {
    return global_unique_lock_type(mutex(), std::defer_lock);
  }

  // Tries to acquire a lock on the glboal critical section.
  // Check owns_lock() to see if the lock was successfully acquired.
  inline global_unique_lock_type TryAcquire() {
    return global_unique_lock_type(mutex(), std::try_to_lock);
  }
};

}  // namespace rex::thread
