//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Copyright (c) 2023 Vinnie Falco (vinnie.falco@gmail.com)
//
// Official repository: https://github.com/cppalliance/mrdox
//

#ifndef MRDOX_SUPPORT_UNLOCK_GUARD_HPP
#define MRDOX_SUPPORT_UNLOCK_GUARD_HPP

#include <mrdox/Platform.hpp>
#include <mutex>

namespace clang {
namespace mrdox {

/** A scoped guard which unlocks a mutex.
*/
class unlock_guard
{
    std::mutex& m_;

public:
    /** Destructor.
    */
    ~unlock_guard()
    {
        m_.lock();
    }

    /** Constructor.
    */
    explicit
    unlock_guard(std::mutex& m)
        : m_(m)
    {
        m_.unlock();
    }
};

} // mrdox
} // clang

#endif
