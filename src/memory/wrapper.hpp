#pragma once

#include <limits>
#include <new>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <utility>

#include "rw_lock.h"
#include "tls_pointer.h"

namespace ipc {
namespace mem {

////////////////////////////////////////////////////////////////
/// Thread-safe allocation wrapper
////////////////////////////////////////////////////////////////

template <typename AllocP>
class synchronized_pool {
public:
    using alloc_policy = AllocP;

private:
    rw_lock lc_;
    std::vector<alloc_policy*> allocs_;

    struct alloc_t {
        synchronized_pool* t_;
        alloc_policy* alc_;

        alloc_t(synchronized_pool* t)
            : t_ { t }
        {}

        ~alloc_t() {

        }
    };

public:
    static constexpr std::size_t remain() {
        return (std::numeric_limits<std::size_t>::max)();
    }

    static void* alloc() {
        static tls::pointer<alloc_policy> alc;
        return nullptr;
    }

    static void* alloc(std::size_t) {
        return alloc();
    }
};

////////////////////////////////////////////////////////////////
/// The allocator wrapper class for STL
////////////////////////////////////////////////////////////////

template <typename T, typename AllocP>
class allocator_wrapper {

    template <typename U, typename AllocU>
    friend class allocator_wrapper;

public:
    // type definitions
    typedef T                 value_type;
    typedef value_type*       pointer;
    typedef const value_type* const_pointer;
    typedef value_type&       reference;
    typedef const value_type& const_reference;
    typedef std::size_t       size_type;
    typedef AllocP            alloc_policy;

private:
    alloc_policy alloc_;

public:
    allocator_wrapper(void) noexcept = default;

    allocator_wrapper(const allocator_wrapper<T, AllocP>& rhs) noexcept
        : alloc_(rhs.alloc_)
    {}

    template <typename U>
    allocator_wrapper(const allocator_wrapper<U, AllocP>& rhs) noexcept
        : alloc_(rhs.alloc_)
    {}

    allocator_wrapper(allocator_wrapper<T, AllocP>&& rhs) noexcept
        : alloc_(std::move(rhs.alloc_))
    {}

    template <typename U>
    allocator_wrapper(allocator_wrapper<U, AllocP>&& rhs) noexcept
        : alloc_(std::move(rhs.alloc_))
    {}

    allocator_wrapper(const AllocP& rhs) noexcept
        : alloc_(rhs)
    {}

    allocator_wrapper(AllocP&& rhs) noexcept
        : alloc_(std::move(rhs))
    {}

public:
    // the other type of std_allocator
    template <typename U>
    struct rebind { typedef allocator_wrapper<U, AllocP> other; };

    constexpr size_type max_size(void) const noexcept {
        return (std::numeric_limits<size_type>::max)() / sizeof(T);
    }

public:
    pointer allocate(size_type count) noexcept {
        if (count == 0) return nullptr;
        if (count > this->max_size()) return nullptr;
        return static_cast<pointer>(alloc_.alloc(count * sizeof(T)));
    }

    void deallocate(pointer p, size_type count) {
        alloc_.free(p, count * sizeof(T));
    }

    template <typename... P>
    static void construct(pointer p, P&&... params) {
        ::new (static_cast<void*>(p)) T(std::forward<P>(params)...);
    }

    static void destroy(pointer p) {
        p->~T();
    }
};

template <class AllocP>
class allocator_wrapper<void, AllocP> {
public:
    typedef void    value_type;
    typedef AllocP  alloc_policy;
};

template <typename T, typename U, class AllocP>
constexpr bool operator==(const allocator_wrapper<T, AllocP>&, const allocator_wrapper<U, AllocP>&) noexcept {
    return true;
}

template <typename T, typename U, class AllocP>
constexpr bool operator!=(const allocator_wrapper<T, AllocP>&, const allocator_wrapper<U, AllocP>&) noexcept {
    return false;
}

} // namespace mem
} // namespace ipc
