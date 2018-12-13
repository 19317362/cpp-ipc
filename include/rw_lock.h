#pragma once

#include <atomic>
#include <limits>

#include "def.h"

namespace ipc {

class rw_lock {
    std::atomic_size_t lc_ { 0 };

    enum : std::size_t {
        w_flag = (std::numeric_limits<std::size_t>::max)()
    };

public:
    void lock() {
        for (unsigned k = 0;; ++k) {
            std::size_t expected = 0;
            if (lc_.compare_exchange_weak(expected, w_flag, std::memory_order_acq_rel)) {
                break;
            }
            yield(k);
        }
    }

    void unlock() {
        lc_.store(0, std::memory_order_release);
    }

    void lock_shared() {
        for (unsigned k = 0;; ++k) {
            std::size_t old = lc_.load(std::memory_order_relaxed);
            std::size_t unlocked = old + 1;
            if (unlocked &&
                lc_.compare_exchange_weak(old, unlocked, std::memory_order_acq_rel)) {
                break;
            }
            yield(k);
            std::atomic_thread_fence(std::memory_order_acquire);
        }
    }

    void unlock_shared() {
        lc_.fetch_sub(1, std::memory_order_release);
    }
};

} // namespace ipc
