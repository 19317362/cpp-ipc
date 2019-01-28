#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <errno.h>

#include <cstring>
#include <atomic>
#include <string>
#include <utility>
#include <tuple>

#include "def.h"
#include "log.h"
#include "shm.h"
#include "rw_lock.h"
#include "id_pool.h"
#include "pool_alloc.h"

#include "platform/detail.h"

namespace ipc {
namespace detail {

class semaphore {
public:
    using handle_t = sem_t*;

    constexpr static handle_t invalid() {
        return SEM_FAILED;
    }

    static handle_t open(char const* name) {
        handle_t sem = ::sem_open(name, O_CREAT, 0666, 0);
        if (sem == SEM_FAILED) {
            ipc::error("fail sem_open[%d]: %s\n", errno, name);
            return invalid();
        }
        return sem;
    }

#pragma push_macro("IPC_SEMAPHORE_FUNC_")
#undef  IPC_SEMAPHORE_FUNC_
#define IPC_SEMAPHORE_FUNC_(CALL, PAR)              \
    if (::CALL(PAR) != 0) {                         \
        ipc::error("fail " #CALL "[%d]\n", errno);  \
        return false;                               \
    }                                               \
    return true

    static bool close(handle_t h) {
        if (h == invalid()) return false;
        IPC_SEMAPHORE_FUNC_(sem_close, h);
    }

    static bool destroy(char const* name) {
        IPC_SEMAPHORE_FUNC_(sem_unlink, name);
    }

    static bool post(handle_t h) {
        if (h == invalid()) return false;
        IPC_SEMAPHORE_FUNC_(sem_post, h);
    }

    static bool wait(handle_t h) {
        if (h == invalid()) return false;
        IPC_SEMAPHORE_FUNC_(sem_wait, h);
    }

#pragma pop_macro("IPC_SEMAPHORE_FUNC_")
};

class event {
    std::atomic<std::size_t>* cnt_ = nullptr;
    semaphore::handle_t sem_ = semaphore::invalid();
    uint_t<16> wait_id_;

    std::string name() const {
        return "__IPC_WAIT__" + std::to_string(wait_id_);
    }

public:
    event(std::size_t id)
        : wait_id_(static_cast<uint_t<16>>(id)) {
        auto n = name();
        cnt_ = static_cast<std::atomic<std::size_t>*>(
                    shm::acquire(n.c_str(), sizeof(std::atomic<std::size_t>)));
        if (cnt_ == nullptr) {
            ipc::error("fail shm::acquire: %s\n", n.c_str());
            return;
        }
        cnt_->fetch_add(1, std::memory_order_acquire);
        sem_ = semaphore::open(n.c_str());
    }

    ~event() {
        semaphore::close(sem_);
        if (cnt_->fetch_sub(1, std::memory_order_release) == 1) {
            semaphore::destroy(name().c_str());
        }
        shm::release(cnt_, sizeof(std::atomic<std::size_t>));
    }

    auto get_id() const noexcept {
        return wait_id_;
    }

    bool wait() {
        if (sem_ == semaphore::invalid()) return false;
        return semaphore::wait(sem_);
    }

    bool notify() {
        if (sem_ == semaphore::invalid()) return false;
        return semaphore::post(sem_);
    }
};

class waiter {
    using evt_id_t = decltype(std::declval<event>().get_id());

    std::atomic<unsigned> counter_ { 0 };
    spin_lock evt_lc_;
    id_pool<sizeof(evt_id_t)> evt_ids_;

    std::size_t push_event(event const & evt) {
        IPC_UNUSED_ auto guard = ipc::detail::unique_lock(evt_lc_);
        std::size_t id = evt_ids_.acquire();
        if (id == invalid_value) {
            ipc::error("fail push_event[has too many waiters]\n");
            return id;
        }
        (*static_cast<evt_id_t*>(evt_ids_.at(id))) = evt.get_id();
        evt_ids_.mark_acquired(id);
        return id;
    }

    void pop_event(std::size_t id) {
        IPC_UNUSED_ auto guard = ipc::detail::unique_lock(evt_lc_);
        evt_ids_.release(id);
    }

public:
    using handle_t = waiter*;

    constexpr static handle_t invalid() {
        return nullptr;
    }

    handle_t open(char const * name) {
        if (name == nullptr || name[0] == '\0') {
            return invalid();
        }
        if (counter_.fetch_add(1, std::memory_order_acq_rel) == 0) {
            evt_ids_.init();
        }
        return this;
    }

    void close(handle_t h) {
        if (h == invalid()) return;
        counter_.fetch_sub(1, std::memory_order_acq_rel);
    }

    static bool wait_all(std::tuple<waiter*, handle_t> const * all, std::size_t size) {
        if (all == nullptr || size == 0) {
            return false;
        }
        // calc a new wait-id & construct event object
        event evt { ipc::detail::calc_unique_id() };
        auto ids = static_cast<std::size_t*>(mem::alloc(sizeof(std::size_t) * size));
        for (std::size_t i = 0; i < size; ++i) {
            ids[i] = std::get<0>(all[i])->push_event(evt);
        }
        IPC_UNUSED_ auto guard = unique_ptr(ids, [all, size](std::size_t* ids) {
            for (std::size_t i = 0; i < size; ++i) {
                std::get<0>(all[i])->pop_event(ids[i]);
            }
            mem::free(ids, sizeof(std::size_t) * size);
        });
        // wait for event signal
        return evt.wait();
    }

    bool wait(handle_t h) {
        if (h == invalid()) return false;
        auto info = std::make_tuple(this, h);
        return wait_all(&info, 1);
    }

    void notify(handle_t h) {
        if (h == invalid()) return;
        IPC_UNUSED_ auto guard = ipc::detail::unique_lock(evt_lc_);
        evt_ids_.for_acquired([this](auto id) {
            event evt { *static_cast<evt_id_t*>(evt_ids_.at(id)) };
            return !evt.notify(); // return if succ
        });
    }

    void broadcast(handle_t h) {
        if (h == invalid()) return;
        IPC_UNUSED_ auto guard = ipc::detail::unique_lock(evt_lc_);
        evt_ids_.for_acquired([this](auto id) {
            event evt { *static_cast<evt_id_t*>(evt_ids_.at(id)) };
            evt.notify();
            return true; // return after all
        });
    }
};

} // namespace detail
} // namespace ipc
