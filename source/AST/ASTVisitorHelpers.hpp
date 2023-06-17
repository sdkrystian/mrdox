// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Copyright (c) 2023 Krystian Stasiowski (sdkrystian@gmail.com)
//
// Official repository: https://github.com/cppalliance/mrdox
//

#ifndef MRDOX_TOOL_AST_ASTVISITORHELPERS_HPP
#define MRDOX_TOOL_AST_ASTVISITORHELPERS_HPP

#include <mrdox/Platform.hpp>
#include <mrdox/Metadata/Record.hpp>
#include <mrdox/Metadata/Namespace.hpp>
#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <type_traits>

#if 0
    #include "Support/Debug.hpp"
    #include "ASTVisitor.hpp"
    #include <clang/AST/Decl.h>
    #include <ranges>
    #include <string>
    #include <utility>
    #include <vector>

    namespace clang {
    namespace mrdox {

    #define TRACE_TRAVERSAL
    #define TRACE_SKIP_UNTIL_EXPLICIT 1
    #define TRACE_SHOW_IMPLICIT 1
    #define TRACE_PRINT_FILE 1
    #define TRACE_NO_PRINT 0

    static
    std::string
    getDeclName(Decl* D)
    {
        if(auto* N = dyn_cast<NamedDecl>(D))
            return N->getQualifiedNameAsString();
        std::vector<std::string> enclosing = {"<unnamed>"};
        for(auto* DC = D->getDeclContext();
            DC && ! DC->isTranslationUnit(); DC = DC->getParent())
        {
            if(auto* N = dyn_cast<NamedDecl>(DC))
                enclosing.insert(enclosing.end(), {"::", N->getNameAsString()});
            else
                enclosing.insert(enclosing.end(), {"::", "<unnamed>"});
        }
        auto v = std::views::join(std::views::reverse(enclosing));
        return std::string(v.begin(), v.end());
    }

    static
    auto
    TracedTraversalState()
    {
        thread_local std::size_t depth = 0;
        thread_local bool saw_explicit =
            ! (TRACE_SKIP_UNTIL_EXPLICIT);
        return std::pair<std::size_t&, bool&>(
            depth, saw_explicit);
    }

    template<
        typename Format,
        typename... Args>
    void print_debug_indented(
        Format fmt,
        std::size_t indent,
        Args&&... args)
    {
        print_debug("{}{}", std::string(indent, ' '), fmt::vformat(
            fmt, fmt::make_format_args(std::forward<Args>(args)...)));
    }

    static
    void
    TracedTraverseContext(
        ASTVisitor& V,
        DeclContext* DC,
        bool increase_depth)
    {
        auto [depth, saw_explicit] = TracedTraversalState();
        if(DC->isTranslationUnit())
        {
            depth = 0;
            saw_explicit = ! (TRACE_SKIP_UNTIL_EXPLICIT);
#if TRACE_PRINT_FILE
            Decl* D = dyn_cast<Decl>(DC);
            SourceManager& src_manager =
                D->getASTContext().getSourceManager();
            print_debug("{}\n",
                src_manager.getNonBuiltinFilenameForID(
                    src_manager.getMainFileID()).value().str());
            ++depth;
#endif
        }
        depth += increase_depth;
        for(auto* C : DC->decls())
        {
#if TRACE_SHOW_IMPLICIT
            if(saw_explicit |= ! C->isImplicit())
#else
            if(! C->isImplicit())
#endif
            {
#if ! TRACE_NO_PRINT
                std::string result = fmt::format(
                    "{:<64}{:<64}",
                    fmt::format(
                        "{}{}{}",
                        std::string(depth * 4, ' '),
                        C->getDeclKindName(),
                        C->isImplicit() ? " (implicit)" : ""),
                    getDeclName(C));
                print_debug("{}\n", result);
#endif
            }
            V.TraverseDecl(C);
        }
        depth -= increase_depth;
    }

    } // mrdox
    } // clang
#endif


namespace clang {
namespace mrdox {

namespace {

AccessKind
convertToAccessKind(
    AccessSpecifier spec)
{
    using OldKind = AccessSpecifier;
    using NewKind = AccessKind;
    switch(spec)
    {
    case OldKind::AS_public:    return NewKind::Public;
    case OldKind::AS_protected: return NewKind::Protected;
    case OldKind::AS_private:   return NewKind::Private;
    case OldKind::AS_none:      return NewKind::None;
    default:
        MRDOX_UNREACHABLE();
    }
}

StorageClassKind
convertToStorageClassKind(
    StorageClass spec)
{
    using OldKind = StorageClass;
    using NewKind = StorageClassKind;
    switch(spec)
    {
    case OldKind::SC_None:     return NewKind::None;
    case OldKind::SC_Extern:   return NewKind::Extern;
    case OldKind::SC_Static:   return NewKind::Static;
    case OldKind::SC_Auto:     return NewKind::Auto;
    case OldKind::SC_Register: return NewKind::Register;
    default:
        // SC_PrivateExtern (__private_extern__)
        // is a C only Apple extension
        MRDOX_UNREACHABLE();
    }
}

ConstexprKind
convertToConstexprKind(
    ConstexprSpecKind spec)
{
    using OldKind = ConstexprSpecKind;
    using NewKind = ConstexprKind;
    switch(spec)
    {
    case OldKind::Unspecified: return NewKind::None;
    case OldKind::Constexpr:   return NewKind::Constexpr;
    case OldKind::Consteval:   return NewKind::Consteval;
    case OldKind::Constinit:   return NewKind::Constinit;
    default:
        MRDOX_UNREACHABLE();
    }
}

ExplicitKind
convertToExplicitKind(
    const ExplicitSpecifier& spec)
{
    using OldKind = ExplicitSpecKind;
    using NewKind = ExplicitKind;

    // no explicit-specifier
    if(! spec.isSpecified())
        return NewKind::None;

    switch(spec.getKind())
    {
    case OldKind::ResolvedFalse:
        return NewKind::ExplicitFalse;
    case OldKind::ResolvedTrue:
        if(spec.getExpr())
            return NewKind::ExplicitTrue;
        // explicit-specifier without constant-expression
        return NewKind::Explicit;
    case OldKind::Unresolved:
        return NewKind::ExplicitUnresolved;
    default:
        MRDOX_UNREACHABLE();
    }
}

NoexceptKind
convertToNoexceptKind(
    ExceptionSpecificationType spec)
{
    using OldKind = ExceptionSpecificationType;
    using NewKind = NoexceptKind;
    switch(spec)
    {
    case OldKind::EST_None:              return NewKind::None;
    case OldKind::EST_DynamicNone:       return NewKind::ThrowNone;
    case OldKind::EST_Dynamic:           return NewKind::Throw;
    case OldKind::EST_MSAny:             return NewKind::ThrowAny;
    case OldKind::EST_NoThrow:           return NewKind::NoThrow;
    case OldKind::EST_BasicNoexcept:     return NewKind::Noexcept;
    case OldKind::EST_DependentNoexcept: return NewKind::NoexceptDependent;
    case OldKind::EST_NoexceptFalse:     return NewKind::NoexceptFalse;
    case OldKind::EST_NoexceptTrue:      return NewKind::NoexceptTrue;
    case OldKind::EST_Unevaluated:       return NewKind::Unevaluated;
    case OldKind::EST_Uninstantiated:    return NewKind::Uninstantiated;
    case OldKind::EST_Unparsed:          return NewKind::Unparsed;
    default:
        MRDOX_UNREACHABLE();
    }
}

OperatorKind
convertToOperatorKind(
    OverloadedOperatorKind kind)
{
    using OldKind = OverloadedOperatorKind;
    using NewKind = OperatorKind;
    switch(kind)
    {
    case OldKind::OO_None:                return NewKind::None;
    case OldKind::OO_New:                 return NewKind::New;
    case OldKind::OO_Delete:              return NewKind::Delete;
    case OldKind::OO_Array_New:           return NewKind::ArrayNew;
    case OldKind::OO_Array_Delete:        return NewKind::ArrayDelete;
    case OldKind::OO_Plus:                return NewKind::Plus;
    case OldKind::OO_Minus:               return NewKind::Minus;
    case OldKind::OO_Star:                return NewKind::Star;
    case OldKind::OO_Slash:               return NewKind::Slash;
    case OldKind::OO_Percent:             return NewKind::Percent;
    case OldKind::OO_Caret:               return NewKind::Caret;
    case OldKind::OO_Amp:                 return NewKind::Amp;
    case OldKind::OO_Pipe:                return NewKind::Pipe;
    case OldKind::OO_Tilde:               return NewKind::Tilde;
    case OldKind::OO_Exclaim:             return NewKind::Exclaim;
    case OldKind::OO_Equal:               return NewKind::Equal;
    case OldKind::OO_Less:                return NewKind::Less;
    case OldKind::OO_Greater:             return NewKind::Greater;
    case OldKind::OO_PlusEqual:           return NewKind::PlusEqual;
    case OldKind::OO_MinusEqual:          return NewKind::MinusEqual;
    case OldKind::OO_StarEqual:           return NewKind::StarEqual;
    case OldKind::OO_SlashEqual:          return NewKind::SlashEqual;
    case OldKind::OO_PercentEqual:        return NewKind::PercentEqual;
    case OldKind::OO_CaretEqual:          return NewKind::CaretEqual;
    case OldKind::OO_AmpEqual:            return NewKind::AmpEqual;
    case OldKind::OO_PipeEqual:           return NewKind::PipeEqual;
    case OldKind::OO_LessLess:            return NewKind::LessLess;
    case OldKind::OO_GreaterGreater:      return NewKind::GreaterGreater;
    case OldKind::OO_LessLessEqual:       return NewKind::LessLessEqual;
    case OldKind::OO_GreaterGreaterEqual: return NewKind::GreaterGreaterEqual;
    case OldKind::OO_EqualEqual:          return NewKind::EqualEqual;
    case OldKind::OO_ExclaimEqual:        return NewKind::ExclaimEqual;
    case OldKind::OO_LessEqual:           return NewKind::LessEqual;
    case OldKind::OO_GreaterEqual:        return NewKind::GreaterEqual;
    case OldKind::OO_Spaceship:           return NewKind::Spaceship;
    case OldKind::OO_AmpAmp:              return NewKind::AmpAmp;
    case OldKind::OO_PipePipe:            return NewKind::PipePipe;
    case OldKind::OO_PlusPlus:            return NewKind::PlusPlus;
    case OldKind::OO_MinusMinus:          return NewKind::MinusMinus;
    case OldKind::OO_Comma:               return NewKind::Comma;
    case OldKind::OO_ArrowStar:           return NewKind::ArrowStar;
    case OldKind::OO_Arrow:               return NewKind::Arrow;
    case OldKind::OO_Call:                return NewKind::Call;
    case OldKind::OO_Subscript:           return NewKind::Subscript;
    case OldKind::OO_Conditional:         return NewKind::Conditional;
    case OldKind::OO_Coawait:             return NewKind::Coawait;
    default:
        MRDOX_UNREACHABLE();
    }
}

ReferenceKind
convertToReferenceKind(
    RefQualifierKind kind)
{
    using OldKind = RefQualifierKind;
    using NewKind = ReferenceKind;
    switch(kind)
    {
    case OldKind::RQ_None:   return NewKind::None;
    case OldKind::RQ_LValue: return NewKind::LValue;
    case OldKind::RQ_RValue: return NewKind::RValue;
    default:
        MRDOX_UNREACHABLE();
    }
}

template<typename T, typename... Args>
void insertChild(NamespaceInfo& I, Args&&... args)
{
    if constexpr(std::is_constructible_v<SymbolID, Args...>)
    {
        if constexpr(T::isField())
            // invalid namespace member
            MRDOX_UNREACHABLE();
        else if constexpr(T::isSpecialization())
            I.Specializations.emplace_back(std::forward<Args>(args)...);
        else
            I.Members.emplace_back(std::forward<Args>(args)...);
    }
    else
    {
        // invalid arguments
        MRDOX_UNREACHABLE();
    }
}

template<typename T, typename... Args>
void insertChild(RecordInfo& I, Args&&... args)
{
    if constexpr(std::is_constructible_v<SymbolID, Args...>)
    {
        if constexpr(T::isSpecialization())
            I.Specializations.emplace_back(std::forward<Args>(args)...);
        else
            I.Members.emplace_back(std::forward<Args>(args)...);
    }
    else
    {
        // invalid arguments
        MRDOX_UNREACHABLE();
    }
}

} // (anon)

} // mrdox
} // clang

#endif
