//
// This is a derivative work. originally part of the LLVM Project.
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Copyright (c) 2023 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2024 Alan de Freitas (alandefreitas@gmail.com)
//
// Official repository: https://github.com/cppalliance/mrdocs
//

#ifndef MRDOCS_LIB_GEN_HTML_HTMLGENERATOR_HPP
#define MRDOCS_LIB_GEN_HTML_HTMLGENERATOR_HPP

#include <mrdocs/Platform.hpp>
#include <mrdocs/Generator.hpp>
#include <lib/Gen/hbs/HandlebarsGenerator.hpp>

namespace clang {
namespace mrdocs {
namespace html {

class HTMLGenerator
    : public hbs::HandlebarsGenerator
{
public:
    HTMLGenerator();

    std::string
    toString(
        hbs::HandlebarsCorpus const&,
        doc::Node const&) const override;

    void
    escape(OutputRef& os, std::string_view str) const override;
};

} // html
} // mrdocs
} // clang

#endif
