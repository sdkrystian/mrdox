//
// This is a derivative work. originally part of the LLVM Project.
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Copyright (c) 2023 Vinnie Falco (vinnie.falco@gmail.com)
//
// Official repository: https://github.com/cppalliance/mrdox
//

#include "lib/Lib/ConfigImpl.hpp"
#include <mrdox/Corpus.hpp>
#include <mrdox/Metadata.hpp>
#include <mrdox/Support/Error.hpp>
#include <ranges>

namespace clang {
namespace mrdox {

//------------------------------------------------

Corpus::~Corpus() noexcept = default;

//------------------------------------------------
//
// Observers
//
//------------------------------------------------

/** Return the metadata for the global namespace.
*/
NamespaceInfo const&
Corpus::
globalNamespace() const noexcept
{
    return get<NamespaceInfo>(SymbolID::zero);
}

//------------------------------------------------
//
// Modifiers
//
//------------------------------------------------

// KRYSTIAN NOTE: temporary
std::string&
Corpus::
getFullyQualifiedName(
    const Info& I,
    std::string& temp) const
{
    temp.clear();
    #if 0
    for(auto const& ns_id : llvm::reverse(I.Namespace))
    {
        if(const Info* ns = find(ns_id))
            temp.append(ns->Name.data(), ns->Name.size());
        else
            temp.append("<unnamed>");

        temp.append("::");
    }
    #else
    for(auto const& parent : std::views::reverse(I.Namespace))
    {
        if(parent)
            temp.append(parent->Name);
        else
            temp.append("<unnamed>");
        temp.append("::");
    }
    #endif
    auto s = I.extractName();
    temp.append(s.data(), s.size());
    return temp;
}


} // mrdox
} // clang
