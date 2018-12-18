#pragma once

#include <atomic>
#include <utility>
#include <algorithm>
#include <thread>

#include "def.h"

namespace ipc {
namespace circ {

template <std::size_t N>
struct alignas(std::max_align_t) elem_array_head {
    using u1_t = uint_t<N>;
    using u2_t = uint_t<N * 2>;

    std::atomic<u2_t> cc_ { 0 }; // connection counter, using for broadcast
    std::atomic<u2_t> wt_ { 0 }; // write index

    static u1_t index_of(u2_t c) { return static_cast<u1_t>(c); }

    std::size_t connect() {
        return cc_.fetch_add(1, std::memory_order_relaxed);
    }

    std::size_t disconnect() {
        return cc_.fetch_sub(1, std::memory_order_relaxed);
    }

    std::size_t conn_count() const {
        return cc_.load(std::memory_order_relaxed);
    }

    u2_t cursor() const {
        return wt_.load(std::memory_order_relaxed);
    }

    auto acquire() {
        return index_of(wt_.load(std::memory_order_acquire));
    }

    void commit() {
        wt_.fetch_add(1, std::memory_order_release);
    }
};

template <std::size_t N>
constexpr std::size_t elem_array_head_size =
    (sizeof(elem_array_head<N>) % alignof(std::max_align_t)) ?
   ((sizeof(elem_array_head<N>) / alignof(std::max_align_t)) + 1) * alignof(std::max_align_t) :
     sizeof(elem_array_head<N>);

struct elem_head {
    std::atomic<uint_t<32>> rc_ { 0 }; // read counter
};

template <std::size_t DataSize, std::size_t BaseIntSize = 8>
class elem_array : private elem_array_head<BaseIntSize> {
public:
    using base_t = elem_array_head<BaseIntSize>;
    using head_t = elem_head;

    using typename base_t::u1_t;
    using typename base_t::u2_t;

    enum : std::size_t {
        head_size  = elem_array_head_size<BaseIntSize>,
        data_size  = DataSize,
        elem_max   = (std::numeric_limits<u1_t>::max)() + 1, // default is 255 + 1
        elem_size  = sizeof(head_t) + DataSize,
        block_size = elem_size * elem_max
    };

private:
    struct elem_t {
        head_t head_;
        byte_t data_[data_size] {};
    };
    elem_t block_[elem_max];

    elem_t* elem_start() {
        return block_;
    }

    static elem_t* elem(void* ptr) { return reinterpret_cast<elem_t*>(static_cast<byte_t*>(ptr) - sizeof(head_t)); }
           elem_t* elem(u1_t i   ) { return elem_start() + i; }

public:
    elem_array() = default;

    elem_array(const elem_array&) = delete;
    elem_array& operator=(const elem_array&) = delete;
    elem_array(elem_array&&) = delete;
    elem_array& operator=(elem_array&&) = delete;

    using base_t::connect;
    using base_t::disconnect;
    using base_t::conn_count;
    using base_t::cursor;

    void* acquire() {
        elem_t* el = elem(base_t::acquire());
        // check all consumers have finished reading
        while(1) {
            uint_t<32> expected = 0;
            if (el->head_.rc_.compare_exchange_weak(
                        expected,
                        static_cast<uint_t<32>>(conn_count()),
                        std::memory_order_relaxed)) {
                break;
            }
            std::this_thread::yield();
            std::atomic_thread_fence(std::memory_order_acquire);
        }
        return el->data_;
    }

    void commit(void* /*ptr*/) {
        base_t::commit();
    }

    void* take(u2_t cursor) {
        return elem(base_t::index_of(cursor))->data_;
    }

    void put(void* ptr) {
        elem(ptr)->head_.rc_.fetch_sub(1, std::memory_order_release);
    }
};

} // namespace circ
} // namespace ipc
