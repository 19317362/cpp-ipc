#pragma once

#include <type_traits>

#include "def.h"
#include "prod_cons.h"

#include "circ/elem_array.h"

namespace ipc {
namespace policy {

template <template <typename, std::size_t...> class Elems,
          typename Flag>
struct choose;

template <typename Flag>
struct choose<circ::elem_array, Flag> {
    using is_fixed = std::true_type;

    template <std::size_t DataSize>
    using elems_t = circ::elem_array<ipc::prod_cons_impl<Flag>, DataSize>;
};

} // namespace policy
} // namespace ipc
