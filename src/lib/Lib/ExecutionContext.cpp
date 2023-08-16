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

#include "ExecutionContext.hpp"
#include "lib/AST/Bitcode.hpp"
#include "lib/Metadata/Reduce.hpp"
#include <mrdox/Metadata.hpp>

namespace clang {
namespace mrdox {

namespace {

// A standalone function to call to merge a vector of infos into one.
// This assumes that all infos in the vector are of the same type, and will fail
// if they are different.
// Dispatch function.
mrdox::Expected<std::unique_ptr<Info>>
mergeInfos(std::vector<std::unique_ptr<Info>>& Values)
{
    if(Values.empty() || ! Values[0])
        return formatError("no info values to merge");

    return visit(*Values[0], [&]<typename T>(T&) mutable
        {
            return reduce<T>(Values);
        });
}

void merge(Info& I, Info&& Other)
{
    MRDOX_ASSERT(I.Kind == Other.Kind);
    visit(I, [&]<typename InfoTy>(InfoTy& II) mutable
        {
            merge(II, static_cast<InfoTy&&>(Other));
        });
}

void resolve(Info& I, UnresolvedInfoSet& Info)
{
    visit(I, [&](auto& II) mutable
        {
            resolve(II, Info);
        });
}

} // (anon)

// ----------------------------------------------------------------
// InfoExecutionContext
// ----------------------------------------------------------------

void
InfoExecutionContext::
report(
    UnresolvedInfoSet&& results,
    Diagnostics&& diags)
{
    InfoSet& infos = results_.get();
    #if 0
    {
        std::shared_lock read_lock(mutex_);

        // resolve any references to Info which
        // exists in the primary result set
        results.resolve(results_);

    }
    #endif


    std::unique_lock write_lock(mutex_);

    results.resolve(results_);
    results_.resolve(results);

    // add all new Info to the existing set. after this call,
    // info will only contain duplicates which will require merging
    infos.merge(results.get());

    for(auto& other : results.get())
    {
        auto it = infos.find(other->id);
        MRDOX_ASSERT(it != infos.end());
        merge(**it, std::move(*other));
    }

    diags_.mergeAndReport(std::move(diags));
}

void
InfoExecutionContext::
reportEnd(report::Level level)
{
    diags_.reportTotals(level);
}


mrdox::Expected<InfoSet>
InfoExecutionContext::
results()
{
    return results_.release();
}

// ----------------------------------------------------------------
// BitcodeExecutionContext
// ----------------------------------------------------------------

void
BitcodeExecutionContext::
report(
    UnresolvedInfoSet&& results,
    Diagnostics&& diags)
{
    // InfoSet info = std::move(results);

    std::lock_guard<std::mutex> lock(mutex_);
    for(auto& I : results.get())
    {
        auto bitcode = writeBitcode(*I);
        auto [it, created] = bitcode_.try_emplace(I->id);
        if(auto& codes = it->second; created ||
            std::find(codes.begin(), codes.end(), bitcode) == codes.end())
            codes.emplace_back(std::move(bitcode));
    }

    diags_.mergeAndReport(std::move(diags));
}

void
BitcodeExecutionContext::
reportEnd(report::Level level)
{
    diags_.reportTotals(level);
}

mrdox::Expected<InfoSet>
BitcodeExecutionContext::
results()
{
    InfoSet result;
    auto errors = config_.threadPool().forEach(
        bitcode_,
        [&](auto& Group)
        {
            // One or more Info for the same symbol ID
            std::vector<std::unique_ptr<Info>> Infos;

            // Each Bitcode can have multiple Infos
            for(auto& bitcode : Group.second)
            {
                auto infos = readBitcode(bitcode);
                std::move(
                    infos->begin(),
                    infos->end(),
                    std::back_inserter(Infos));
            }

            auto merged = mergeInfos(Infos);
            std::unique_ptr<Info> I = merged.release();
            MRDOX_ASSERT(I);
            MRDOX_ASSERT(Group.first == I->id);
            std::lock_guard<std::mutex> lock(mutex_);
            result.emplace(std::move(I));
        });

    if(! errors.empty())
        return Error(errors);
    return result;
}


} // mrdox
} // clang
