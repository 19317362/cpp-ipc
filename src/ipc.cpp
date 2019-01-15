#include "ipc.h"

#include <type_traits>
#include <cstring>
#include <algorithm>
#include <utility>
#include <atomic>

#include "def.h"
#include "shm.h"
#include "tls_pointer.h"

#include "memory/resource.hpp"

namespace {

using namespace ipc;
using data_t   = byte_t[data_length];
using msg_id_t = std::size_t;

inline auto acc_of_msg() {
    static shm::handle g_shm { "GLOBAL_ACC_STORAGE__", sizeof(std::atomic<msg_id_t>) };
    return static_cast<std::atomic<msg_id_t>*>(g_shm.get());
}

template <template <typename...> class Queue, typename Policy>
struct detail;

template <typename Policy>
struct detail<ipc::queue, Policy> {

#pragma pack(1)
struct msg_t {
    void*    que_;
    msg_id_t id_;
    int      remain_;
    data_t   data_;
};
#pragma pack()

using queue_t = ipc::queue<msg_t, Policy>;

struct shm_info_t {
    typename queue_t::elems_t elems_;  // the circ_elem_array in shm
};

constexpr static void* head_of(queue_t* que) {
    return static_cast<void*>(que->elems());
}

constexpr static queue_t* queue_of(handle_t h) {
    return static_cast<queue_t*>(h);
}

static buff_t make_cache(void const * data, std::size_t size) {
    auto ptr = mem::detail::pool_alloc::alloc(size);
    std::memcpy(ptr, data, size);
    return { ptr, size, mem::detail::pool_alloc::free };
}

struct cache_t {
    std::size_t fill_;
    buff_t      buff_;

    cache_t(std::size_t f, buff_t&& b)
        : fill_(f), buff_(std::move(b))
    {}

    void append(void const * data, std::size_t size) {
        std::memcpy(static_cast<byte_t*>(buff_.data()) + fill_, data, size);
        fill_ += size;
    }
};

static auto& recv_cache() {
    /*
        <Remarks> thread_local may have some bugs.
        See: https://sourceforge.net/p/mingw-w64/bugs/727/
             https://sourceforge.net/p/mingw-w64/bugs/527/
             https://github.com/Alexpux/MINGW-packages/issues/2519
             https://github.com/ChaiScript/ChaiScript/issues/402
             https://developercommunity.visualstudio.com/content/problem/124121/thread-local-variables-fail-to-be-initialized-when.html
             https://software.intel.com/en-us/forums/intel-c-compiler/topic/684827
    */
    static tls::pointer<mem::unordered_map<msg_id_t, cache_t>> rc;
    return *rc.create();
}

static auto& queues_cache() {
    static tls::pointer<mem::vector<queue_t*>> qc;
    return *qc.create();
}

/* API implementations */

static handle_t connect(char const * name) {
    auto mem = shm::acquire(name, sizeof(shm_info_t));
    if (mem == nullptr) {
        return nullptr;
    }
    return new queue_t { &(static_cast<shm_info_t*>(mem)->elems_) };
}

static void disconnect(handle_t h) {
    queue_t* que = queue_of(h);
    if (que == nullptr) {
        return;
    }
    que->disconnect(); // needn't to detach, cause it will be deleted soon.
    shm::release(head_of(que), sizeof(shm_info_t));
    delete que;
}

static std::size_t recv_count(handle_t h) {
    auto que = queue_of(h);
    if (que == nullptr) {
        return invalid_value;
    }
    return que->conn_count();
}

static void wait_for_recv(handle_t h, std::size_t r_count) {
    for (unsigned k = 0; recv_count(h) < r_count;) {
        ipc::sleep(k);
    }
}

static void clear_recv(handle_t h) {
    auto* head = head_of(queue_of(h));
    if (head == nullptr) {
        return;
    }
    std::memset(head, 0, sizeof(shm_info_t));
}

static void clear_recv(char const * name) {
    auto h = connect(name);
    clear_recv(h);
    disconnect(h);
}

static bool send(handle_t h, void const * data, std::size_t size) {
    if (data == nullptr) {
        return false;
    }
    if (size == 0) {
        return false;
    }
    auto que = queue_of(h);
    if (que == nullptr) {
        return false;
    }
    // calc a new message id, start with 1
    auto msg_id = acc_of_msg()->fetch_add(1, std::memory_order_relaxed) + 1;
    // push message fragment, one fragment size is data_length
    int offset = 0;
    for (int i = 0; i < static_cast<int>(size / data_length); ++i, offset += data_length) {
        msg_t msg {
            que, msg_id,
            static_cast<int>(size) - offset - static_cast<int>(data_length), {}
        };
        std::memcpy(msg.data_, static_cast<byte_t const *>(data) + offset, data_length);
        if (!que->push(msg)) return false;
    }
    // if remain > 0, this is the last message fragment
    int remain = static_cast<int>(size) - offset;
    if (remain > 0) {
        msg_t msg {
            que, msg_id,
            remain - static_cast<int>(data_length), {}
        };
        std::memcpy(msg.data_, static_cast<byte_t const *>(data) + offset,
                               static_cast<std::size_t>(remain));
        if (!que->push(msg)) return false;
    }
    return true;
}

static buff_t recv(handle_t h) {
    auto que = queue_of(h);
    if (que == nullptr) return {};
    que->connect(); // wouldn't connect twice
    auto& rc = recv_cache();
    while (1) {
        // pop a new message
        auto msg = que->pop();
        if (msg.id_ == 0) return {};
        if (msg.que_ == que) continue; // pop next
        // msg.remain_ may minus & abs(msg.remain_) < data_length
        std::size_t remain = static_cast<std::size_t>(
                             static_cast<int>(data_length) + msg.remain_);
        // find cache with msg.id_
        auto cac_it = rc.find(msg.id_);
        if (cac_it == rc.end()) {
            if (remain <= data_length) {
                return make_cache(msg.data_, remain);
            }
            // cache the first message fragment
            else rc.try_emplace(msg.id_, data_length, make_cache(msg.data_, remain));
        }
        // has cached before this message
        else {
            auto& cac = cac_it->second;
            // this is the last message fragment
            if (msg.remain_ <= 0) {
                cac.append(msg.data_, remain);
                // finish this message, erase it from cache
                auto buff = std::move(cac.buff_);
                rc.erase(cac_it);
                return buff;
            }
            // there are remain datas after this message
            cac.append(msg.data_, data_length);
        }
    }
}

}; // detail<ipc::queue>

} // internal-linkage

namespace ipc {

template <template <typename...> class Queue, typename Policy>
handle_t channel_detail<Queue, Policy>::connect(char const * name) {
    return detail<Queue, Policy>::connect(name);
}

template <template <typename...> class Queue, typename Policy>
void channel_detail<Queue, Policy>::disconnect(handle_t h) {
    detail<Queue, Policy>::disconnect(h);
}

template <template <typename...> class Queue, typename Policy>
std::size_t channel_detail<Queue, Policy>::recv_count(handle_t h) {
    return detail<Queue, Policy>::recv_count(h);
}

template <template <typename...> class Queue, typename Policy>
void channel_detail<Queue, Policy>::wait_for_recv(handle_t h, std::size_t r_count) {
    return detail<Queue, Policy>::wait_for_recv(h, r_count);
}

template <template <typename...> class Queue, typename Policy>
void channel_detail<Queue, Policy>::clear_recv(handle_t h) {
    detail<Queue, Policy>::clear_recv(h);
}

template <template <typename...> class Queue, typename Policy>
void channel_detail<Queue, Policy>::clear_recv(char const * name) {
    detail<Queue, Policy>::clear_recv(name);
}

template <template <typename...> class Queue, typename Policy>
bool channel_detail<Queue, Policy>::send(handle_t h, void const * data, std::size_t size) {
    return detail<Queue, Policy>::send(h, data, size);
}

template <template <typename...> class Queue, typename Policy>
buff_t channel_detail<Queue, Policy>::recv(handle_t h) {
    return detail<Queue, Policy>::recv(h);
}

template struct channel_detail<ipc::queue, ipc::prod_cons_circ<relat::single, relat::single, trans::unicast  >>;
template struct channel_detail<ipc::queue, ipc::prod_cons_circ<relat::single, relat::multi , trans::unicast  >>;
template struct channel_detail<ipc::queue, ipc::prod_cons_circ<relat::multi , relat::multi , trans::unicast  >>;
template struct channel_detail<ipc::queue, ipc::prod_cons_circ<relat::single, relat::multi , trans::broadcast>>;
template struct channel_detail<ipc::queue, ipc::prod_cons_circ<relat::multi , relat::multi , trans::broadcast>>;

} // namespace ipc
