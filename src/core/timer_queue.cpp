/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <condition_variable>
#include <forward_list>
#include <mutex>

#include <disruptorplus/multi_threaded_claim_strategy.hpp>
#include <disruptorplus/ring_buffer.hpp>
#include <disruptorplus/sequence_barrier.hpp>

#include <rex/assert.h>
#include <rex/platform.h>
#include <rex/thread.h>
#include <rex/thread/timer_queue.h>

#if REX_PLATFORM_WIN32
#include "platform_win.h"

#include <timeapi.h>
#endif

namespace dp = disruptorplus;

namespace rex::thread {

using WaitItem = TimerQueueWaitItem;

// A disruptorplus wait strategy that blocks on a condition variable. disruptorplus ships one
// (blocking_wait_strategy), but its timed waits pass the predicate before the timeout to
// std::condition_variable and do not compile.
class BlockingWaitStrategy {
 public:
  dp::sequence_t wait_until_published(dp::sequence_t sequence, size_t count,
                                      const std::atomic<dp::sequence_t>* const sequences[]) {
    dp::sequence_t result;
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] {
      result = dp::minimum_sequence_after(sequence, count, sequences);
      return dp::difference(result, sequence) >= 0;
    });
    return result;
  }

  template <typename Clock, typename Duration>
  dp::sequence_t wait_until_published(dp::sequence_t sequence, size_t count,
                                      const std::atomic<dp::sequence_t>* const sequences[],
                                      const std::chrono::time_point<Clock, Duration>& timeout) {
    dp::sequence_t result;
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_until(lock, timeout, [&] {
      result = dp::minimum_sequence_after(sequence, count, sequences);
      return dp::difference(result, sequence) >= 0;
    });
    return result;
  }

  void signal_all_when_blocking() {
    // Under the mutex, so a waiter between its check and its wait cannot miss the signal.
    std::lock_guard<std::mutex> lock(mutex_);
    cv_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
};

class TimerQueue {
 public:
  using clock = WaitItem::clock;
  static_assert(clock::is_steady);

 public:
  TimerQueue()
      : buffer_(kWaitCount),
        wait_strategy_(),
        claim_strategy_(kWaitCount, wait_strategy_),
        consumed_(wait_strategy_) {
#if REX_PLATFORM_WIN32
    // The dispatch thread sleeps on a condition variable, whose timeouts Windows rounds up to
    // the system timer period (15.6 ms by default). The 1 ms KeTimeStampBundle timer needs 1 ms.
    timer_period_set_ = timeBeginPeriod(1) == TIMERR_NOERROR;
#endif
    claim_strategy_.add_claim_barrier(consumed_);
    dispatch_thread_ =
        std::jthread([this](std::stop_token stop_token) { TimerThreadMain(stop_token); });
  }

  ~TimerQueue() {
    dispatch_thread_.request_stop();

    // Kick dispatch thread to check stop token
    auto wait_item = std::make_shared<WaitItem>(nullptr, nullptr, this, clock::time_point::min(),
                                                clock::duration::zero());
    wait_item->Disarm();
    QueueTimer(std::move(wait_item));
    dispatch_thread_.join();

#if REX_PLATFORM_WIN32
    if (timer_period_set_) {
      timeEndPeriod(1);
    }
#endif
  }

  void TimerThreadMain(std::stop_token stop_token) {
    dp::sequence_t next_sequence = 0;
    const auto comp = [](const std::shared_ptr<WaitItem>& left,
                         const std::shared_ptr<WaitItem>& right) {
      return left->due_ < right->due_;
    };

    set_current_thread_name("rex::thread::TimerQueue");

    while (!stop_token.stop_requested()) {
      {
        // Sleep until the earliest timer is due or a new one is queued, then add new wait
        // items to the sorted wait queue.
        dp::sequence_t available =
            wait_queue_.empty()
                ? claim_strategy_.wait_until_published(next_sequence, next_sequence - 1)
                : claim_strategy_.wait_until_published(next_sequence, next_sequence - 1,
                                                       wait_queue_.front()->due_);

        // Check for timeout
        if (available != next_sequence - 1) {
          std::forward_list<std::shared_ptr<WaitItem>> wait_items;
          do {
            wait_items.push_front(std::move(buffer_[next_sequence]));
          } while (next_sequence++ != available);

          consumed_.publish(available);

          wait_items.sort(comp);
          wait_queue_.merge(wait_items, comp);
        }
      }

      {
        // Check wait queue, invoke callbacks and reschedule
        std::forward_list<std::shared_ptr<WaitItem>> wait_items;
        while (!wait_queue_.empty() && wait_queue_.front()->due_ <= clock::now()) {
          auto wait_item = std::move(wait_queue_.front());
          wait_queue_.pop_front();

          // Ensure that it isn't disarmed
          auto state = WaitItem::State::kIdle;
          if (wait_item->state_.compare_exchange_strong(state, WaitItem::State::kInCallback,
                                                        std::memory_order_acq_rel)) {
            // Possibility to dispatch to a thread pool here
            assert_not_null(wait_item->callback_);
            wait_item->callback_(wait_item->userdata_);

            if (wait_item->interval_ != clock::duration::zero() &&
                wait_item->state_.load(std::memory_order_acquire) !=
                    WaitItem::State::kInCallbackSelfDisarmed) {
              // Item is recurring and didn't self-disarm during callback:
              wait_item->due_ += wait_item->interval_;
              wait_item->state_.store(WaitItem::State::kIdle, std::memory_order_release);
              wait_item->state_.notify_all();
              wait_items.push_front(std::move(wait_item));
            } else {
              wait_item->state_.store(WaitItem::State::kDisarmed, std::memory_order_release);
              wait_item->state_.notify_all();
            }
          } else {
            // Specifically, kInCallback is illegal here
            assert_true(WaitItem::State::kDisarmed == state);
          }
        }
        wait_items.sort(comp);
        wait_queue_.merge(wait_items, comp);
      }
    }
  }

  std::weak_ptr<WaitItem> QueueTimer(std::shared_ptr<WaitItem> wait_item) {
    auto wait_item_weak = std::weak_ptr<WaitItem>(wait_item);

    // Mitigate callback flooding
    wait_item->due_ = std::max(clock::now() - wait_item->interval_, wait_item->due_);

    auto sequence = claim_strategy_.claim_one();
    buffer_[sequence] = std::move(wait_item);
    claim_strategy_.publish(sequence);

    return wait_item_weak;
  }

  std::jthread::id dispatch_thread_id() const { return dispatch_thread_.get_id(); }

 private:
  // This ring buffer will be used to introduce timers queued by the public API
  static constexpr size_t kWaitCount = 512;
  dp::ring_buffer<std::shared_ptr<WaitItem>> buffer_;
  // Blocking rather than spinning: the queue holds a few timers firing at most every 1 ms, and
  // a spinning dispatch thread keeps a host core busy for nothing.
  BlockingWaitStrategy wait_strategy_;
  dp::multi_threaded_claim_strategy<BlockingWaitStrategy> claim_strategy_;
  dp::sequence_barrier<BlockingWaitStrategy> consumed_;
#if REX_PLATFORM_WIN32
  bool timer_period_set_ = false;
#endif

  // This is a _sorted_ (ascending due_) list of active timers managed by a
  // dedicated thread
  std::forward_list<std::shared_ptr<WaitItem>> wait_queue_;
  std::jthread dispatch_thread_;
};

rex::thread::TimerQueue timer_queue_;

void TimerQueueWaitItem::Disarm() {
  State state;

  // Special case for calling from a callback itself
  if (std::this_thread::get_id() == parent_queue_->dispatch_thread_id()) {
    state = State::kInCallback;
    if (state_.compare_exchange_strong(state, State::kInCallbackSelfDisarmed,
                                       std::memory_order_acq_rel)) {
      // If we are self disarming from the callback set this special state and
      // exit
      return;
    }
    // Normal case can handle the rest
  }

  state = State::kIdle;
  // Classes which hold WaitItems will often call Disarm() to cancel them during
  // destruction. This may lead to race conditions when the dispatch thread
  // executes a callback which accesses memory that is freed simultaneously due
  // to this. Therefore, we need to guarantee that no callbacks will be running
  // once Disarm() has returned.
  while (!state_.compare_exchange_weak(state, State::kDisarmed, std::memory_order_acq_rel)) {
    if (state == State::kDisarmed) {
      break;
    }
    if (state == State::kInCallback || state == State::kInCallbackSelfDisarmed) {
      // Wait for callback to complete - dispatch thread will notify
      state_.wait(state, std::memory_order_acquire);
    }
    state = State::kIdle;
  }
}

std::weak_ptr<WaitItem> QueueTimerOnce(std::function<void(void*)> callback, void* userdata,
                                       WaitItem::clock::time_point due) {
  return timer_queue_.QueueTimer(std::make_shared<WaitItem>(
      std::move(callback), userdata, &timer_queue_, due, WaitItem::clock::duration::zero()));
}

std::weak_ptr<WaitItem> QueueTimerRecurring(std::function<void(void*)> callback, void* userdata,
                                            WaitItem::clock::time_point due,
                                            WaitItem::clock::duration interval) {
  return timer_queue_.QueueTimer(
      std::make_shared<WaitItem>(std::move(callback), userdata, &timer_queue_, due, interval));
}

}  // namespace rex::thread
