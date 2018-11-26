#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <utility>
#include <limits>
#include <algorithm>
#include <thread>

namespace ipc {

using byte_t = std::uint8_t;

namespace circ {

struct alignas(std::max_align_t) elem_array_head {
    using ui_t = std::uint8_t;
    using uc_t = std::uint16_t;
    using ai_t = std::atomic<ui_t>;
    using ac_t = std::atomic<uc_t>;

    ac_t cc_ { 0 }; // connection counter, using for broadcast
    ac_t cr_ { 0 }; // cursor
    ai_t wt_ { 0 }; // write index
};

enum : std::size_t {
    elem_array_head_size =
        (sizeof(elem_array_head) % alignof(std::max_align_t)) ?
       ((sizeof(elem_array_head) / alignof(std::max_align_t)) + 1) * alignof(std::max_align_t) :
         sizeof(elem_array_head)
};

template <std::size_t DataSize>
class elem_array : private elem_array_head {
    struct head_t {
        ac_t rf_;              // read flag
        std::atomic_bool wf_;  // write flag
        std::atomic_flag acq_; // acquire flag
    };

public:
    enum : std::size_t {
        head_size  = elem_array_head_size,
        data_size  = DataSize,
        elem_max   = std::numeric_limits<ui_t>::max() + 1, // default is 255 + 1
        elem_size  = sizeof(head_t) + DataSize,
        block_size = elem_size * elem_max
    };

    static_assert(data_size % alignof(head_t) == 0, "data_size must be multiple of alignof(head_t)");

private:
    struct elem_t {
        head_t head_;
        byte_t data_[data_size];
    };
    elem_t block_[elem_max];

    elem_t* elem_start(void) {
        return block_;
    }

    static elem_t* elem(void* ptr) {
        return reinterpret_cast<elem_t*>(static_cast<byte_t*>(ptr) - sizeof(head_t));
    }

    elem_t* elem(ui_t i) {
        return elem_start() + i;
    }

    static ui_t index_of(uc_t c) {
        return static_cast<ui_t>(c & std::numeric_limits<ui_t>::max());
    }

    ui_t index_of(elem_t* el) {
        return static_cast<ui_t>(el - elem_start());
    }

public:
    elem_array(void) {
        ::memset(block_, 0, sizeof(block_));
    }
    ~elem_array(void) = delete;

    elem_array(const elem_array&) = delete;
    elem_array& operator=(const elem_array&) = delete;
    elem_array(elem_array&&) = delete;
    elem_array& operator=(elem_array&&) = delete;

    std::size_t connect(void) {
        return cc_.fetch_add(1, std::memory_order_release);
    }

    std::size_t disconnect(void) {
        return cc_.fetch_sub(1, std::memory_order_release);
    }

    std::size_t conn_count(void) const {
        return cc_.load(std::memory_order_consume);
    }

    void* acquire(void) {
        elem_t* el;
        while (1) {
            // searching an available element
            el = elem(wt_.fetch_add(1, std::memory_order_acquire));
            if (el->head_.acq_.test_and_set(std::memory_order_release)) {
                std::this_thread::yield();
                continue;
            }
            // check read finished by all consumers
            while(1) {
                uc_t expected = 0;
                std::atomic_thread_fence(std::memory_order_acquire);
                if (el->head_.rf_.compare_exchange_weak(
                            expected, cc_.load(std::memory_order_relaxed), std::memory_order_release)) {
                    break;
                }
                std::this_thread::yield();
            }
            el->head_.acq_.clear(std::memory_order_release);
            break;
        }
        return el->data_;
    }

    void commit(void* ptr) {
        auto el = elem(ptr);    // get the commit element
        ui_t wi = index_of(el); // get the index of this element (the write index)
        do {
            bool no_next, cas;
            uc_t curr = cr_.load(std::memory_order_consume), next;
            do {
                next = curr;
                if ((no_next = (index_of(curr) != wi)) /* assign & judge */) {
                    /*
                     * commit is not the current commit
                     * set wf_ for the other producer thread which is commiting
                     * the element matches cr_ could see it has commited
                    */
                    el->head_.wf_.store(true, std::memory_order_release);
                }
                else {
                    /*
                     * commit is the current commit
                     * so we just increase the cursor & go check the next
                    */
                    ++next;
                    el->head_.wf_.store(false, std::memory_order_release);
                }
                /*
                 * it needs to go back and judge again
                 * when cr_ has been changed by the other producer thread
                */
            } while(!(cas = cr_.compare_exchange_weak(curr, next, std::memory_order_acq_rel)) && no_next);
            /*
             * if compare_exchange failed & !no_next,
             * means there is another producer thread updated this commit,
             * so in this case we could just return
            */
            if (no_next || (!cas/* && !no_next*/)) return;
            /*
             * check next element has commited or not
            */
        } while(el = elem(++wi), el->head_.wf_.exchange(false, std::memory_order_acq_rel));
    }

    uc_t cursor(void) const {
        return cr_.load(std::memory_order_consume);
    }

    void* take(uc_t cursor) {
        return elem(index_of(cursor))->data_;
    }

    void put(void* ptr) {
        elem(ptr)->head_.rf_.fetch_sub(1, std::memory_order_release);
    }
};

} // namespace circ
} // namespace ipc
