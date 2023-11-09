//
// This is a derivative work. originally part of the LLVM Project.
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Copyright (c) 2023 Krystian Stasiowski (sdkrystian@gmail.com)
//
// Official repository: https://github.com/cppalliance/mrdocs
//

#ifndef MRDOCS_LIB_TOOL_SYMBOLS_HPP
#define MRDOCS_LIB_TOOL_SYMBOLS_HPP

#include <mrdocs/Platform.hpp>
#include <mrdocs/Metadata/Info.hpp>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>

namespace clang {
namespace mrdocs {

#if 0
class InfoContext;

using SymbolIDImpl = struct SymbolID::Impl
{
    using StorageType = std::array<std::uint8_t, 20>;

    const InfoContext& context_;
    StorageType data_;

    Impl(
        const InfoContext& context,
        const StorageType& data)
        : context_(context)
        , data_(data)
    {

    }
    SymbolID toSymbolID() const noexcept
    {
        return SymbolID(this);
    }

    struct Hash
    {
        using is_transparent = void;

        std::size_t operator()(const StorageType&) const;

        std::size_t operator()(const std::unique_ptr<Impl>&) const;
    };

    struct Equal
    {
        using is_transparent = void;

        bool operator()(
            const std::unique_ptr<Impl>& a,
            const std::unique_ptr<Impl>& b) const;

        bool operator()(
            const std::unique_ptr<Impl>& a,
            const StorageType& b) const;

        bool operator()(
            const StorageType& a,
            const std::unique_ptr<Impl>& b) const;
    };
};

class SymbolIDRegistry
{
    using RawID = SymbolIDImpl::StorageType;

    std::unordered_set<
        std::unique_ptr<SymbolIDImpl>,
        SymbolIDImpl::Hash,
        SymbolIDImpl::Equal> symbols_;
    const InfoContext& context_;
    SymbolIDImpl global_namespace_;
    std::shared_mutex mutex_;

public:
    SymbolIDRegistry(const InfoContext& context);

    SymbolID get(const RawID& id);
};
#endif

} // mrdocs
} // clang

#endif
