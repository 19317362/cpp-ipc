#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "platform/waiter.h"

namespace ipc {

template <typename U2>
struct conn_head {
    ipc::detail::waiter cc_waiter_;
    std::atomic<U2>     cc_ { 0 }; // connection counter

    auto       & conn_waiter()       { return this->cc_waiter_; }
    auto const & conn_waiter() const { return this->cc_waiter_; }

    std::size_t connect() noexcept {
        return cc_.fetch_add(1, std::memory_order_release);
    }

    std::size_t disconnect() noexcept {
        return cc_.fetch_sub(1, std::memory_order_release);
    }

    std::size_t conn_count(std::memory_order order = std::memory_order_acquire) const noexcept {
        return cc_.load(order);
    }
};

} // namespace ipc
