//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Copyright (c) 2023 Vinnie Falco (vinnie.falco@gmail.com)
//
// Official repository: https://github.com/cppalliance/mrdox
//

#ifndef MRDOX_TOOL_SUPPORT_TYPE_TRAITS_HPP
#define MRDOX_TOOL_SUPPORT_TYPE_TRAITS_HPP

#include <concepts>
#include <type_traits>
#include <utility>
#include <functional>

namespace clang {
namespace mrdox {

/** Return the value as its underlying type.
*/
template<class Enum>
requires std::is_enum_v<Enum>
constexpr auto
to_underlying(
    Enum value) noexcept ->
    std::underlying_type_t<Enum>
{
    return static_cast<
        std::underlying_type_t<Enum>>(value);
}

// ----------------------------------------------------------------

template<typename From, typename To>
struct add_lvalue_reference_from : std::conditional<
    std::is_lvalue_reference_v<From>,
        std::add_lvalue_reference_t<To>, To> { };

template<typename From, typename To>
using add_lvalue_reference_from_t =
    add_lvalue_reference_from<From, To>::type;

// ----------------------------------------------------------------

template<typename From, typename To>
struct add_rvalue_reference_from : std::conditional<
    std::is_rvalue_reference_v<From>,
        std::add_rvalue_reference_t<To>, To> { };

template<typename From, typename To>
using add_rvalue_reference_from_t =
    add_rvalue_reference_from<From, To>::type;

// ----------------------------------------------------------------

template<typename From, typename To>
struct add_reference_from
    : add_lvalue_reference_from<From,
        add_rvalue_reference_from_t<From, To>> { };

template<typename From, typename To>
using add_reference_from_t =
    add_reference_from<From, To>::type;

// ----------------------------------------------------------------

template<typename From, typename To>
struct add_const_from : std::conditional<
    std::is_const_v<std::remove_reference_t<From>>,
        add_reference_from_t<To, const std::remove_reference_t<To>>, To> { };

template<typename From, typename To>
using add_const_from_t =
    add_const_from<From, To>::type;

// ----------------------------------------------------------------

template<typename From, typename To>
struct add_volatile_from : std::conditional<
    std::is_volatile_v<std::remove_reference_t<From>>,
        add_reference_from_t<To, volatile std::remove_reference_t<To>>, To> { };


template<typename From, typename To>
using add_volatile_from_t =
    add_volatile_from<From, To>::type;

// ----------------------------------------------------------------

template<typename From, typename To>
struct add_cv_from
    : add_const_from<From,
        add_volatile_from_t<From, To>> { };

template<typename From, typename To>
using add_cv_from_t =
    add_cv_from<From, To>::type;

// ----------------------------------------------------------------

template<typename From, typename To>
struct add_cvref_from
    : add_reference_from<From,
        add_cv_from_t<From, To>> { };

template<typename From, typename To>
using add_cvref_from_t =
    add_cvref_from<From, To>::type;

// ----------------------------------------------------------------

template<typename F, typename... Args>
void
invoke_if_valid(F&&, Args&&...) { }

template<typename F, typename... Args>
    requires std::is_invocable_v<F, Args...>
decltype(auto)
invoke_if_valid(F&& f, Args&&... args)
{
    return std::invoke(
        std::forward<F>(f),
        std::forward<Args>(args)...);
}

template<typename R, typename F, typename... Args>
void
invoke_r_if_valid(F&&, Args&&...) { }

template<typename R, typename F, typename... Args>
    requires std::is_invocable_r_v<R, F, Args...>
decltype(auto)
invoke_r_if_valid(F&& f, Args&&... args)
{
    // KRYSTIAN NOTE: std::invoke_r is c++23
    return static_cast<R>(
        std::invoke(
            std::forward<F>(f),
            std::forward<Args>(args)...));
}

template<typename... Args>
struct overloaded : Args...
{
    using Args::operator()...;
};

} // mrdox
} // clang

#endif
