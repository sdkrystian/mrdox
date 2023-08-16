//
// This is a derivative work. originally part of the LLVM Project.
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Copyright (c) 2023 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2023 Krystian Stasiowski (sdkrystian@gmail.com)
//
// Official repository: https://github.com/cppalliance/mrdox
//

#ifndef MRDOX_LIB_TOOL_INFO_HPP
#define MRDOX_LIB_TOOL_INFO_HPP

#include <mrdox/Platform.hpp>
#include <mrdox/Metadata/Info.hpp>
#include <memory>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

namespace clang {
namespace mrdox {

struct InfoPtrHasher
{
    using is_transparent = void;

    std::size_t operator()(
        const std::unique_ptr<Info>& I) const;

    std::size_t operator()(
        const SymbolID& id) const;
};

struct InfoPtrEqual
{
    using is_transparent = void;

    bool operator()(
        const std::unique_ptr<Info>& a,
        const std::unique_ptr<Info>& b) const;

    bool operator()(
        const std::unique_ptr<Info>& a,
        const SymbolID& b) const;

    bool operator()(
        const SymbolID& a,
        const std::unique_ptr<Info>& b) const;
};

using InfoSet = std::unordered_set<
    std::unique_ptr<Info>, InfoPtrHasher, InfoPtrEqual>;

#if 0
template<typename Key>
class UnresolvedInfo
{
    InfoSet& info_;
    std::unordered_map<Key, void*> unresolved_;

public:
    UnresolvedInfo(InfoSet& info)
        : info_(info)
    {
    }

    #if 1
    Info*
    find(
        const Key& key,
        Info*& dst)
    #else
    template<typename InfoTy>
        requires std::same_as<
            std::remove_cv_t<InfoTy>, Info>
    InfoTy*
    find(
        const Key& key,
        InfoTy*& dst)
    #endif
    {
        // if the Info is already in the Info set,
        // write the pointer value to the destination
        if(auto it = info_.find(key);
            it != info_.end())
        {
            dst = it->get();
            return dst;
        }

        // otherwise, add the reference to the unresolved map.
        // the destination is used to store a pointer to the next
        // unresolved reference, forming a linked list which is
        // traversed & written with the Info* once it is known
        if(auto [it, created] = unresolved_.emplace(
            key, &dst); ! created)
        {
            dst = static_cast<Info*>(
            // dst = static_cast<InfoTy*>(
                std::exchange(it->second, &dst));
        }
        return nullptr;
    }

    Info*
    find(
        const Key& key,
        const Info*& dst)
    {
        return find(key, const_cast<Info*&>(dst));
    }

    void
    emplace(
        const Key& key,
        std::unique_ptr<Info>&& info)
    {
        // update the Info set
        Info* ptr = info_.emplace(
            std::move(info)).first->get();
        // if there are no unresolved references
        // to this Info, we are done
        auto it = unresolved_.find(key);
        if(it == unresolved_.end())
            return;
        // walk the linked list of unresolved Info*,
        // and write the now known pointer value
        for(void* head = it->second; head;)
            head = std::exchange(
                *static_cast<Info**>(head), ptr);
        // remove the entry from the unresolved map
        unresolved_.erase(it);
    }

    void
    emplace(std::unique_ptr<Info>&& info)
        requires std::same_as<Key, SymbolID>
    {
        MRDOX_ASSERT(info);
        return emplace(
            info->id, std::move(info));
    }
};
#else

inline
bool
isUnresolved(
    const Info* info) noexcept
{
    return reinterpret_cast<
        std::uintptr_t>(info) & 1ull;
}

class UnresolvedInfoSet
{
    InfoSet info_;
    std::unordered_map<SymbolID, Info**> unresolved_;


    template<typename T>
    T* set_unresolved(T* ptr)
    {
        return reinterpret_cast<T*>(
            reinterpret_cast<std::uintptr_t>(ptr) | 1ull);
    }

    template<typename T>
    T* clear_unresolved(T* ptr)
    {
        return reinterpret_cast<T*>(
            reinterpret_cast<std::uintptr_t>(ptr) & ~1ull);
    }

    template<typename T>
    bool is_unresolved(T* ptr)
    {
        return reinterpret_cast<T*>(
            reinterpret_cast<std::uintptr_t>(ptr) & 1ull);
    }

    void
    resolve(
        Info* info,
        bool erase)
    {
        MRDOX_ASSERT(info);
        // if there are no unresolved references
        // to this Info, we are done
        auto it = unresolved_.find(info->id);
        if(it == unresolved_.end())
            return;
        // walk the linked list of unresolved Info*,
        // and write the now known pointer value
        for(Info** head = it->second; head;)
            head = reinterpret_cast<Info**>(
                std::exchange(*clear_unresolved(head), info));
        // erase the entry from the unresolved map
        if(erase)
            unresolved_.erase(it);
        else
            it->second = nullptr;
    }

public:
    InfoSet& get()
    {
        return info_;
    }

    InfoSet release()
    {
        return std::move(info_);
    }

    Info*
    find(const SymbolID& id)
    {
        if(auto it = info_.find(id); it != info_.end())
            return it->get();
        return nullptr;
    }

    Info*
    find(
        const SymbolID& id,
        Info*& dst)
    {
        // if the Info is already in the Info set,
        // write the pointer value to the destination
        if(Info* info = find(id))
        {
            dst = info;
            return info;
        }

        // otherwise, add the reference to the unresolved map.
        // the destination is used to store a pointer to the next
        // unresolved reference, forming a linked list which is
        // traversed & written with the Info* once it is known
#if 0
        if(auto [it, created] = unresolved_.emplace(
            id, &dst); ! created)
        {
            dst = reinterpret_cast<Info*>(
                std::exchange(it->second, &dst));
        }
#else
        auto dst_val = set_unresolved(&dst);
        if(auto [it, created] = unresolved_.emplace(
            id, dst_val); ! created)
            dst = reinterpret_cast<Info*>(
                std::exchange(it->second, dst_val));
#endif
        return nullptr;
    }

    Info*
    find(
        const SymbolID& id,
        const Info*& dst)
    {
        return find(id, const_cast<Info*&>(dst));
    }

    Info*
    emplace(std::unique_ptr<Info>&& ptr)
    {
        MRDOX_ASSERT(ptr);
        // update the Info set
        Info* info = info_.emplace(
            std::move(ptr)).first->get();
        // update any unresolved references
        resolve(info);
        return info;
    }

    void
    resolve(Info* info)
    {
        return resolve(info, true);
    }

    void
    resolve(UnresolvedInfoSet& other)
    {
        // resolve any references to Info
        // which exists in the other set
        for(const SymbolID& id :
            std::views::keys(unresolved_))
        {
            if(Info* info = other.find(id))
                resolve(info, false);
        }
        // erase entries which were resolved
        std::erase_if(unresolved_,
            [](const auto& kv)
            {
                return ! kv.second;
            });
    }

    auto
    unresolved()
    {
        return std::views::keys(unresolved_);
    }
};

#endif

} // mrdox
} // clang

#endif
