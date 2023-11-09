//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Copyright (c) 2023 Krystian Stasiowski (sdkrystian@gmail.com)
//
// Official repository: https://github.com/cppalliance/mrdocs
//

#include "Info.hpp"
#include "lib/Support/Radix.hpp"
// #include <functional>
#include <mutex>

#if 0
std::size_t
std::hash<clang::mrdocs::SymbolID>::
operator()(const clang::mrdocs::SymbolID& id) const
{
    return std::hash<const clang::mrdocs::SymbolID::Impl*>()(id.impl_);
}
#endif
namespace clang {
namespace mrdocs {

std::size_t
InfoPtrHasher::
operator()(
    const std::unique_ptr<Info>& I) const
{
    // the info set should never contain nullptrs
    MRDOCS_ASSERT(I);
    return std::hash<SymbolID>()(I->id);
}

std::size_t
InfoPtrHasher::
operator()(
    const SymbolID& id) const
{
    return std::hash<SymbolID>()(id);
}

bool
InfoPtrEqual::
operator()(
    const std::unique_ptr<Info>& a,
    const std::unique_ptr<Info>& b) const
{
    MRDOCS_ASSERT(a && b);
    if(a == b)
        return true;
    return a->id == b->id;
}

bool
InfoPtrEqual::
operator()(
    const std::unique_ptr<Info>& a,
    const SymbolID& b) const
{
    MRDOCS_ASSERT(a);
    return a->id == b;
}

bool
InfoPtrEqual::
operator()(
    const SymbolID& a,
    const std::unique_ptr<Info>& b) const
{
    MRDOCS_ASSERT(b);
    return b->id == a;
}

std::size_t
SymbolID::
Impl::
Hash::
operator()(const StorageType& data) const
{
    return std::hash<std::string_view>()(std::string_view(
        reinterpret_cast<const char*>(data.data()), data.size()));
}

std::size_t
SymbolID::
Impl::
Hash::
operator()(const std::unique_ptr<Impl>& I) const
{
    MRDOCS_ASSERT(I);
    return (*this)(I->data_);
}

bool
SymbolID::
Impl::
Equal::
operator()(
    const std::unique_ptr<Impl>& a,
    const std::unique_ptr<Impl>& b) const
{
    MRDOCS_ASSERT(a && b);
    if(a == b)
        return true;
    return a->data_ == b->data_;
}

bool
SymbolID::
Impl::
Equal::
operator()(
    const std::unique_ptr<Impl>& a,
    const StorageType& b) const
{
    MRDOCS_ASSERT(a);
    return a->data_ == b;
}

bool
SymbolID::
Impl::
Equal::
operator()(
    const StorageType& a,
    const std::unique_ptr<Impl>& b) const
{
    MRDOCS_ASSERT(b);
    return b->data_ == a;
}







std::span<const std::uint8_t, 20>
SymbolID::value() const noexcept
{
    constexpr static Impl::StorageType zeroed = {};
    if(! valid())
        return zeroed;
    // MRDOCS_ASSERT(impl_->context_);
    return impl_->data_;
}

const Info&
SymbolID::operator*() const noexcept
{
    MRDOCS_ASSERT(valid());
    auto it = impl_->context_.info().find(*this);
    MRDOCS_ASSERT(it != impl_->context_.info().end());
    return **it;
}

const Info*
SymbolID::operator->() const noexcept
{
    MRDOCS_ASSERT(valid());
    auto it = impl_->context_.info().find(*this);
    MRDOCS_ASSERT(it != impl_->context_.info().end());
    return it->get();
}

std::string
toString(
    SymbolID id,
    unsigned radix,
    bool lowercase)
{
    std::string_view bytes(reinterpret_cast<const char*>(
        id.value().data()), id.value().size());
    if(radix == 16)
        return toBase16(bytes, lowercase);
    else if(radix == 64)
        return toBase64(bytes);
    MRDOCS_UNREACHABLE();
}



InfoContext::
InfoContext()
    : global_(*this, []()
        {
            RawID global_id;
            global_id.fill(0xFF);
            return global_id;
        }())
{
}

SymbolID
InfoContext::
globalNamespaceID()
{
    return global_.toSymbolID();
}

SymbolID
InfoContext::
getSymbolID(const RawID& id)
{
    // don't access the set if the ID is invalid
    // is the or the ID of the global namespace
    if(id == RawID())
        return SymbolID();
    if(id == global_.data_)
        return globalNamespaceID();

    {
        // see if the ID already exists
        std::shared_lock<std::shared_mutex> read_lock(symbols_mutex_);
        // if the ID already exists, return the SymbolID for that entry
        if(auto it = symbols_.find(id);
            it != symbols_.end())
            return it->get()->toSymbolID();
    }

    // otherwise, insert the new entry
    std::unique_lock<std::shared_mutex> write_lock(symbols_mutex_);
    auto [it, created] = symbols_.emplace(
        std::make_unique<SymbolIDImpl>(*this, id));
    return it->get()->toSymbolID();
}

} // mrdocs
} // clang
