//
// This is a derivative work. originally part of the LLVM Project.
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Copyright (c) 2023 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2023 Krystian Stasiowski (sdkrystian@gmail.com)
//
// Official repository: https://github.com/cppalliance/mrdocs
//

#ifndef MRDOCS_API_METADATA_SYMBOLS_HPP
#define MRDOCS_API_METADATA_SYMBOLS_HPP

#include <mrdocs/Platform.hpp>
#include <mrdocs/ADT/Optional.hpp>
#include <cstdint>
#include <cstring>
#include <span>
#include <compare>
#include <string_view>

namespace clang {
namespace mrdocs {

struct Info;

/** A unique identifier for a symbol.

    This is calculated as the SHA1 digest of the
    USR. A USRs is a string that provide an
    unambiguous reference to a symbol.
*/
class SymbolID
{
public:
    static const SymbolID invalid;

    constexpr SymbolID() noexcept = default;

    std::span<const std::uint8_t, 20>
    value() const noexcept;

    constexpr bool valid() const noexcept
    {
        return *this != SymbolID::invalid;
    }

    constexpr explicit operator bool() const noexcept
    {
        return valid();
    }

    const Info& operator*() const noexcept;

    const Info* operator->() const noexcept;

    constexpr bool operator==(
        const SymbolID&) const noexcept = default;

private:
    friend std::hash<SymbolID>;

    struct Impl;

    constexpr SymbolID(const Impl* impl) noexcept
        : impl_(impl)
    {
    }

    const Impl* impl_ = nullptr;
};

/** The invalid Symbol ID.
*/
// KRYSTIAN NOTE: msvc requires inline as it doesn't consider this
// to be an inline variable without it (it should; see [dcl.constexpr])
constexpr inline SymbolID SymbolID::invalid = SymbolID();

MRDOCS_DECL
std::string
toString(
    SymbolID id,
    unsigned radix,
    bool lowercase = false);


/** Return the result of comparing s0 to s1.

    This function returns true if the string
    s0 is less than the string s1. The comparison
    is first made without regard to case, unless
    the strings compare equal and then they
    are compared with lowercase letters coming
    before uppercase letters.
*/
MRDOCS_DECL
std::strong_ordering
compareSymbolNames(
    std::string_view symbolName0,
    std::string_view symbolName1) noexcept;

} // mrdocs
} // clang

template<>
struct std::hash<clang::mrdocs::SymbolID>
{
    std::size_t operator()(const clang::mrdocs::SymbolID& id) const
    {
        using SymbolID = clang::mrdocs::SymbolID;
        return std::hash<const SymbolID::Impl*>()(id.impl_);
        // return std::hash<std::string_view>()(
        //     std::string_view(id));
    }
    #if 0
    #endif
};

#endif
