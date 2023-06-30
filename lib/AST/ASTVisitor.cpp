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

#include "ASTVisitor.hpp"
#include "ASTVisitorHelpers.hpp"
#include "Bitcode.hpp"
#include "ParseJavadoc.hpp"
#include "Tool/ConfigImpl.hpp"
#include "Support/Path.hpp"
#include "Support/Debug.hpp"
#include <mrdox/Metadata.hpp>
#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclFriend.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Index/USRGeneration.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/SHA1.h>
#include <ranges>

namespace clang {
namespace mrdox {

std::size_t
InfoPtrHasher::
operator()(
    const InfoPtr& I) const
{
    // the info set should never contain nullptrs
    MRDOX_ASSERT(I);
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
    const InfoPtr& a,
    const InfoPtr& b) const
{
    MRDOX_ASSERT(a && b);
    if(a == b)
        return true;
    return a->id == b->id;
}

bool
InfoPtrEqual::
operator()(
    const InfoPtr& a,
    const SymbolID& b) const
{
    MRDOX_ASSERT(a);
    return a->id == b;
}

bool
InfoPtrEqual::
operator()(
    const SymbolID& a,
    const InfoPtr& b) const
{
    MRDOX_ASSERT(b);
    return b->id == a;
}

//------------------------------------------------
//
// ASTVisitor
//
//------------------------------------------------

ASTVisitor::
ASTVisitor(
    tooling::ExecutionContext& ex,
    ConfigImpl const& config,
    clang::CompilerInstance& compiler) noexcept
    : ex_(static_cast<ExecutionContext&>(ex))
    , config_(config)
    , compiler_(compiler)
    , IsFileInRootDir_(true)
{
}

//------------------------------------------------

Info*
ASTVisitor::
getInfo(const SymbolID& id)
{
    if(auto it = info_.find(id); it != info_.end())
        return it->get();
    return nullptr;
}

template<typename InfoTy>
InfoTy&
ASTVisitor::
createInfo(const SymbolID& id)
{
    auto [it, inserted] = info_.emplace(
        std::make_unique<InfoTy>(id));
    MRDOX_ASSERT(inserted);
    return static_cast<InfoTy&>(*it->get());
}

template<typename InfoTy>
std::pair<InfoTy&, bool>
ASTVisitor::
getOrCreateInfo(const SymbolID& id)
{
    if(Info* info = getInfo(id))
    {
        MRDOX_ASSERT(info->Kind == InfoTy::kind_id);
        return {static_cast<InfoTy&>(*info), false};
    }
    return {createInfo<InfoTy>(id), true};
}

Info&
ASTVisitor::
getOrBuildInfo(Decl* D)
{
    SymbolID id = extractSymbolID(D);
    if(Info* info = getInfo(id))
        return *info;

    // KRYSTIAN FIXME: this is terrible
    bool force = forceExtract_;
    forceExtract_ = true;
    traverseDecl(D);
    forceExtract_ = force;

    MRDOX_ASSERT(getInfo(id));
    return *getInfo(id);
}

//------------------------------------------------

// Function to hash a given USR value for storage.
// As USRs (Unified Symbol Resolution) could bef
// large, especially for functions with long type
// arguments, we use 160-bits SHA1(USR) values to
// guarantee the uniqueness of symbols while using
// a relatively small amount of memory (vs storing
// USRs directly).
//
bool
ASTVisitor::
extractSymbolID(
    const Decl* D,
    SymbolID& id)
{
    // functions require their parameter types to be decayed
    // prior to USR generator to ensure that declarations
    // with parameter types which decay to the same type
    // generate the same USR
    if(const auto* FD = dyn_cast<FunctionDecl>(D))
        applyDecayToParameters(FD);
    usr_.clear();
    if(index::generateUSRForDecl(D, usr_))
        return false;
    id = SymbolID(llvm::SHA1::hash(
        arrayRefFromStringRef(usr_)).data());
    return true;
}

SymbolID
ASTVisitor::
extractSymbolID(
    const Decl* D)
{
    SymbolID id = SymbolID::zero;
    extractSymbolID(D, id);
    return id;
}

bool
ASTVisitor::
shouldSerializeInfo(
    const NamedDecl* D) noexcept
{
    // KRYSTIAN FIXME: getting the access of a members
    // is not as simple as calling Decl::getAccessUnsafe.
    // specifically, templates may not have
    // their access set until they are actually instantiated.
    return true;
#if 0
    if(config_.includePrivate)
        return true;
    if(IsOrIsInAnonymousNamespace)
        return false;
    // bool isPublic()
    AccessSpecifier access = D->getAccessUnsafe();
    if(access == AccessSpecifier::AS_private)
        return false;
    Linkage linkage = D->getLinkageInternal();
    if(linkage == Linkage::ModuleLinkage ||
        linkage == Linkage::ExternalLinkage)
        return true;
    // some form of internal linkage
    return false;
#endif
}

//------------------------------------------------

unsigned
ASTVisitor::
getLine(
    const NamedDecl* D) const
{
    return sourceManager_->getPresumedLoc(
        D->getBeginLoc()).getLine();
}

void
ASTVisitor::
addSourceLocation(
    SourceInfo& I,
    unsigned line,
    bool definition)
{
    if(definition)
    {
        if(I.DefLoc)
            return;
        I.DefLoc.emplace(line, File_.str(), IsFileInRootDir_);
    }
    else
    {
        auto existing = std::find_if(I.Loc.begin(), I.Loc.end(),
            [this, line](const Location& l)
            {
                return l.LineNumber == line &&
                    l.Filename == File_.str();
            });
        if(existing != I.Loc.end())
            return;
        I.Loc.emplace_back(line, File_.str(), IsFileInRootDir_);
    }
}

std::string
ASTVisitor::
getSourceCode(
    SourceRange const& R)
{
    return Lexer::getSourceText(
        CharSourceRange::getTokenRange(R),
        *sourceManager_,
        astContext_->getLangOpts()).str();
}

//------------------------------------------------

std::string
ASTVisitor::
getTypeAsString(
    QualType T)
{
    return T.getAsString(astContext_->getPrintingPolicy());
}

template<typename TypeInfoTy>
std::unique_ptr<TypeInfoTy>
ASTVisitor::
makeTypeInfo(
    const IdentifierInfo* II,
    unsigned quals)
{
    auto I = std::make_unique<TypeInfoTy>();
    I->CVQualifiers = convertToQualifierKind(quals);
    if(II)
        I->Name = II->getName();
    return I;
}

static
NamedDecl*
lookupTypedefInPrimary(
    TypedefNameDecl* TD)
{
    if(auto* R = dyn_cast<CXXRecordDecl>(
        TD->getDeclContext()))
    {
        if(CXXRecordDecl* IP = R->getTemplateInstantiationPattern())
            R = IP;
        if(DeclarationName TDN = TD->getDeclName();
            R && ! TDN.isEmpty())
        {
            auto found = R->lookup(TDN);
            MRDOX_ASSERT(found.isSingleResult());
            MRDOX_ASSERT(isa<TypedefNameDecl>(found.front()) ||
                isa<TypeAliasTemplateDecl>(found.front()));
            return found.front();
        }
    }
    return nullptr;
}

template<typename TypeInfoTy>
std::unique_ptr<TypeInfoTy>
ASTVisitor::
makeTypeInfo(
    NamedDecl* N,
    unsigned quals)
{
    auto I = std::make_unique<TypeInfoTy>();
    I->CVQualifiers = convertToQualifierKind(quals);
    if(! N)
        return I;
    if(const auto* II = N->getIdentifier())
        I->Name = II->getName();
    // do not generate references to implicit declarations,
    // template template parameters, or builtin templates
    if(! N->isImplicit() &&
        ! isa<TemplateTemplateParmDecl>(N) &&
        ! isa<BuiltinTemplateDecl>(N))
    {
        if(auto* R = dyn_cast<CXXRecordDecl>(N))
        {
            if(auto* P = R->getTemplateInstantiationPattern())
                N = P;
        }
        else if(auto* TD = dyn_cast<TypedefNameDecl>(N))
        {
            if(auto* PTD = lookupTypedefInPrimary(TD))
                N = PTD;
        }
        else if(auto* ATD = dyn_cast<TypeAliasTemplateDecl>(N))
        {
            if(auto* MT = ATD->getInstantiatedFromMemberTemplate())
                ATD = MT;
            auto* TD = ATD->getTemplatedDecl();
            if(auto* R = dyn_cast<CXXRecordDecl>(TD->getDeclContext()))
            {
            // KRYSTIAN FIXME: this appears to not work
            if(auto* PATD = lookupTypedefInPrimary(TD))
                N = PATD;
            }
        }

        extractSymbolID(N, I->id);
        getOrBuildInfo(N);
    }
    return I;
}

std::unique_ptr<TypeInfo>
ASTVisitor::
buildTypeInfo(
    const NestedNameSpecifier* N)
{
    if(N)
    {
        if(const auto* T = N->getAsType())
            return buildTypeInfo(QualType(T, 0));
        if(const auto* I = N->getAsIdentifier())
        {
            auto R = std::make_unique<TagTypeInfo>();
            R->ParentType = buildTypeInfo(N->getPrefix());
            R->Name = I->getName();
            return R;
        }
    }
    return nullptr;
}

std::unique_ptr<TypeInfo>
ASTVisitor::
buildTypeInfo(
    QualType qt,
    unsigned quals)
{
    qt.addFastQualifiers(quals);
    // should never be called for a QualType
    // that has no Type pointer
    MRDOX_ASSERT(! qt.isNull());
    const Type* type = qt.getTypePtr();
    quals = qt.getLocalFastQualifiers();
    switch(qt->getTypeClass())
    {
    // parenthesized types
    case Type::Paren:
    {
        auto* T = cast<ParenType>(type);
        return buildTypeInfo(
            T->getInnerType(), quals);
    }
    // type with __atribute__
    case Type::Attributed:
    {
        auto* T = cast<AttributedType>(type);
        return buildTypeInfo(
            T->getModifiedType(), quals);
    }
    // adjusted and decayed types
    case Type::Decayed:
    case Type::Adjusted:
    {
        auto* T = cast<AdjustedType>(type);
        return buildTypeInfo(
            T->getOriginalType(), quals);
    }
    // using declarations
    case Type::Using:
    {
        auto* T = cast<UsingType>(type);
        // look through the using declaration and
        // use the the type from the referenced declaration
        return buildTypeInfo(
            T->getUnderlyingType(), quals);
    }
    // pointers
    case Type::Pointer:
    {
        auto* T = cast<PointerType>(type);
        auto I = std::make_unique<PointerTypeInfo>();
        I->PointeeType = buildTypeInfo(
            T->getPointeeType());
        I->CVQualifiers = convertToQualifierKind(quals);
        return I;
    }
    // references
    case Type::LValueReference:
    {
        auto* T = cast<LValueReferenceType>(type);
        auto I = std::make_unique<LValueReferenceTypeInfo>();
        I->PointeeType = buildTypeInfo(
            T->getPointeeType());
        return I;
    }
    case Type::RValueReference:
    {
        auto* T = cast<RValueReferenceType>(type);
        auto I = std::make_unique<RValueReferenceTypeInfo>();
        I->PointeeType = buildTypeInfo(
            T->getPointeeType());
        return I;
    }
    // pointer to members
    case Type::MemberPointer:
    {
        auto* T = cast<MemberPointerType>(type);
        auto I = std::make_unique<MemberPointerTypeInfo>();
        I->PointeeType = buildTypeInfo(
            T->getPointeeType());
        I->ParentType = buildTypeInfo(
            QualType(T->getClass(), 0));
        I->CVQualifiers = convertToQualifierKind(quals);
        return I;
    }
    // pack expansion
    case Type::PackExpansion:
    {
        auto* T = cast<PackExpansionType>(type);
        auto I = std::make_unique<PackTypeInfo>();
        I->PatternType = buildTypeInfo(T->getPattern());
        return I;
    }
    // KRYSTIAN NOTE: we don't handle FunctionNoProto here,
    // and it's unclear if we should. we should not encounter
    // such types in c++ (but it might be possible?)
    // functions
    case Type::FunctionProto:
    {
        auto* T = cast<FunctionProtoType>(type);
        auto I = std::make_unique<FunctionTypeInfo>();
        I->ReturnType = buildTypeInfo(
            T->getReturnType());
        for(QualType PT : T->getParamTypes())
            I->ParamTypes.emplace_back(
                buildTypeInfo(PT));
        I->RefQualifier = convertToReferenceKind(
            T->getRefQualifier());
        I->CVQualifiers = convertToQualifierKind(
            T->getMethodQuals().getFastQualifiers());
        I->ExceptionSpec = convertToNoexceptKind(
            T->getExceptionSpecType());
        return I;
    }
    // KRYSTIAN FIXME: do we handle variables arrays?
    // they can only be created within function scope
    // arrays
    case Type::IncompleteArray:
    {
        auto* T = cast<IncompleteArrayType>(type);
        auto I = std::make_unique<ArrayTypeInfo>();
        I->ElementType = buildTypeInfo(
            T->getElementType());
        return I;
    }
    case Type::ConstantArray:
    {
        auto* T = cast<ConstantArrayType>(type);
        auto I = std::make_unique<ArrayTypeInfo>();
        I->ElementType = buildTypeInfo(
            T->getElementType());
        // KRYSTIAN FIXME: this is broken; cannonical
        // constant array types never have a size expression
        buildExprInfo(I->Bounds,
            T->getSizeExpr(), T->getSize());
        return I;
    }
    case Type::DependentSizedArray:
    {
        auto* T = cast<DependentSizedArrayType>(type);
        auto I = std::make_unique<ArrayTypeInfo>();
        I->ElementType = buildTypeInfo(
            T->getElementType());
        buildExprInfo(I->Bounds,
            T->getSizeExpr());
        return I;
    }
    case Type::Auto:
    {
        auto* T = cast<AutoType>(type);
        QualType deduced = T->getDeducedType();
        // KRYSTIAN NOTE: we don't use isDeduced because it will
        // return true if the type is dependent
        // if the type has been deduced, use the deduced type
        if(! deduced.isNull())
            return buildTypeInfo(deduced);
        auto I = std::make_unique<BuiltinTypeInfo>();
        I->Name = getTypeAsString(
            qt.withoutLocalFastQualifiers());
        I->CVQualifiers = convertToQualifierKind(quals);
        return I;
    }
    case Type::DeducedTemplateSpecialization:
    {
        auto* T = cast<DeducedTemplateSpecializationType>(type);
        if(T->isDeduced())
            return buildTypeInfo(T->getDeducedType());
        auto I = makeTypeInfo<TagTypeInfo>(
            T->getTemplateName().getAsTemplateDecl(), quals);
        return I;
    }
    // elaborated type specifier or
    // type with nested name specifier
    case Type::Elaborated:
    {
        auto* T = cast<ElaboratedType>(type);
        auto I = buildTypeInfo(
            T->getNamedType(), quals);
        // ignore elaborated-type-specifiers
        if(auto kw = T->getKeyword();
            kw != ElaboratedTypeKeyword::ETK_Typename &&
            kw != ElaboratedTypeKeyword::ETK_None)
            return I;
        switch(I->Kind)
        {
        case TypeKind::Tag:
            static_cast<TagTypeInfo&>(*I).ParentType =
                buildTypeInfo(T->getQualifier());
            break;
        case TypeKind::Specialization:
            static_cast<SpecializationTypeInfo&>(*I).ParentType =
                buildTypeInfo(T->getQualifier());
            break;
        case TypeKind::Builtin:
            // KRYSTIAN FIXME: is this correct?
            break;
        default:
            MRDOX_UNREACHABLE();
        };
        return I;
    }
    // qualified dependent name with template keyword
    case Type::DependentTemplateSpecialization:
    {
        auto* T = cast<DependentTemplateSpecializationType>(type);
        auto I = makeTypeInfo<SpecializationTypeInfo>(
            T->getIdentifier(), quals);
        I->ParentType = buildTypeInfo(
            T->getQualifier());
        buildTemplateArgs(I->TemplateArgs, T->template_arguments());
        return I;
    }
    // specialization of a class/alias template or
    // template template parameter
    case Type::TemplateSpecialization:
    {
        auto* T = cast<TemplateSpecializationType>(type);
        auto name = T->getTemplateName();
        MRDOX_ASSERT(! name.isNull());
        auto I = makeTypeInfo<SpecializationTypeInfo>(
            name.getAsTemplateDecl(), quals);
        buildTemplateArgs(I->TemplateArgs, T->template_arguments());
        return I;
    }
    // dependent typename-specifier
    case Type::DependentName:
    {
        auto* T = cast<DependentNameType>(type);
        auto I = makeTypeInfo<TagTypeInfo>(
            T->getIdentifier(), quals);
        I->ParentType = buildTypeInfo(
            T->getQualifier());
        return I;
    }
    // record & enum types, as well as injected class names
    // within a class template (or specializations thereof)
    case Type::InjectedClassName:
    case Type::Record:
    case Type::Enum:
    {
        auto I = makeTypeInfo<TagTypeInfo>(
            type->getAsTagDecl(), quals);
        return I;
    }
    // typedef/alias type
    case Type::Typedef:
    {
        auto* T = cast<TypedefType>(type);
        auto I = makeTypeInfo<TagTypeInfo>(
            T->getDecl(), quals);
        return I;
    }
    case Type::TemplateTypeParm:
    {
        auto* T = cast<TemplateTypeParmType>(type);
        auto I = std::make_unique<BuiltinTypeInfo>();
        if(auto* D = T->getDecl())
        {
            // special case for implicit template parameters
            // resulting from abbreviated function templates
            if(D->isImplicit())
                I->Name = "auto";
            else if(auto* II = D->getIdentifier())
                I->Name = II->getName();
        }
        I->CVQualifiers = convertToQualifierKind(quals);
        return I;
    }
    // builtin/unhandled type
    default:
    {
        auto I = std::make_unique<BuiltinTypeInfo>();
        I->Name = getTypeAsString(
            qt.withoutLocalFastQualifiers());
        I->CVQualifiers = convertToQualifierKind(quals);
        return I;
    }
    }
}

template<typename Integer>
Integer
ASTVisitor::
getValue(const llvm::APInt& V)
{
    if constexpr(std::is_signed_v<Integer>)
        return static_cast<Integer>(
            V.getSExtValue());
    else
        return static_cast<Integer>(
            V.getZExtValue());
}

void
ASTVisitor::
buildExprInfo(
    ExprInfo& I,
    const Expr* E)
{
    if(! E)
        return;
    I.Written = getSourceCode(
        E->getSourceRange());
}

template<typename T>
void
ASTVisitor::
buildExprInfo(
    ConstantExprInfo<T>& I,
    const Expr* E)
{
    buildExprInfo(
        static_cast<ExprInfo&>(I), E);
    // if the expression is dependent,
    // we cannot get its value
    if(! E || E->isValueDependent())
        return;
    I.Value.emplace(getValue<T>(
        E->EvaluateKnownConstInt(*astContext_)));
}

template<typename T>
void
ASTVisitor::
buildExprInfo(
    ConstantExprInfo<T>& I,
    const Expr* E,
    const llvm::APInt& V)
{
    buildExprInfo(I, E);
    I.Value.emplace(getValue<T>(V));
}

TParam
ASTVisitor::
buildTemplateParam(
    const NamedDecl* ND)
{
    // KRYSTIAN NOTE: Decl::isParameterPack
    // returns true for function parameter packs
    TParam info(
        ND->getNameAsString(),
        ND->isTemplateParameterPack());

    if(const auto* TP = dyn_cast<
        TemplateTypeParmDecl>(ND))
    {
        auto& extinfo = info.emplace<
            TypeTParam>();
        if(TP->hasDefaultArgument())
        {
            extinfo.Default = buildTypeInfo(
                TP->getDefaultArgument());
        }
    }
    else if(const auto* TP = dyn_cast<
        NonTypeTemplateParmDecl>(ND))
    {
        auto& extinfo = info.emplace<
            NonTypeTParam>();
        extinfo.Type = buildTypeInfo(
            TP->getType());
        if(TP->hasDefaultArgument())
        {
            extinfo.Default.emplace(getSourceCode(
                TP->getDefaultArgumentLoc()));
        }
    }
    else if(const auto* TP = dyn_cast<
        TemplateTemplateParmDecl>(ND))
    {
        auto& extinfo = info.emplace<
            TemplateTParam>();
        const auto* NestedParamList = TP->getTemplateParameters();
        for(const NamedDecl* NND : *NestedParamList)
        {
            extinfo.Params.emplace_back(
                buildTemplateParam(NND));
        }
        if(TP->hasDefaultArgument())
        {
            extinfo.Default.emplace(getSourceCode(
                TP->getDefaultArgumentLoc()));
        }
    }
    return info;
}

template<typename Range>
void
ASTVisitor::
buildTemplateArgs(
    std::vector<TArg>& result,
    Range&& range)
{
    // TypePrinter generates an internal placeholder name (e.g. type-parameter-0-0)
    // for template type parameters used as arguments. it also cannonicalizes
    // types, which we do not want (although, PrintingPolicy has an option to change this).
    // thus, we use the template arguments as written.

    // KRYSTIAN NOTE: this can probably be changed to select
    // the argument as written when it is not dependent and is a type.
    // FIXME: constant folding behavior should be consistent with that of other
    // constructs, e.g. noexcept specifiers & explicit specifiers
    const auto& policy = astContext_->getPrintingPolicy();
    for(const TemplateArgument& arg : range)
    {
        std::string arg_str;
        if(arg.getKind() == TemplateArgument::Type)
        {
            QualType qt = arg.getAsType();
            // KRYSTIAN FIXME: we *really* should not be
            // converting types to strings like this.
            // TArg needs to be a variant type anyways.
            arg_str = toString(*buildTypeInfo(qt));
        }
        else
        {
            llvm::raw_string_ostream stream(arg_str);
            arg.print(policy, stream, false);
        }
        result.emplace_back(std::move(arg_str));
    }
}

void
ASTVisitor::
parseTemplateArgs(
    TemplateInfo& I,
    const ClassTemplateSpecializationDecl* spec)
{
    // KRYSTIAN FIXME: should this use getTemplateInstantiationPattern?
    // ID of the primary template
    if(ClassTemplateDecl* primary = spec->getSpecializedTemplate())
    {
        if(auto* MT = primary->getInstantiatedFromMemberTemplate())
            primary = MT;
        extractSymbolID(primary, I.Primary.emplace());
    }
    // KRYSTIAN NOTE: when this is a partial specialization, we could use
    // ClassTemplatePartialSpecializationDecl::getTemplateArgsAsWritten
    const TypeSourceInfo* type_written = spec->getTypeAsWritten();
    // if the type as written is nullptr (it should never be), bail
    if(! type_written)
        return;
    auto args = type_written->getType()->getAs<
        TemplateSpecializationType>()->template_arguments();
    buildTemplateArgs(I.Args, args);
}

void
ASTVisitor::
parseTemplateArgs(
    TemplateInfo& I,
    const VarTemplateSpecializationDecl* spec)
{
    // KRYSTIAN FIXME: should this use getTemplateInstantiationPattern?
    // ID of the primary template
    if(VarTemplateDecl* primary = spec->getSpecializedTemplate())
    {
        if(auto* MT = primary->getInstantiatedFromMemberTemplate())
            primary = MT;
        // unlike function and class templates, the USR generated
        // for variable templates differs from that of the VarDecl
        // returned by getTemplatedDecl. this might be a clang bug.
        // the USR of the templated VarDecl seems to be the correct one.
        extractSymbolID(primary->getTemplatedDecl(), I.Primary.emplace());
    }
    const ASTTemplateArgumentListInfo* args_written = nullptr;
    // getTemplateArgsInfo returns nullptr for partial specializations,
    // so we use getTemplateArgsAsWritten if this is a partial specialization
    if(auto* partial = dyn_cast<VarTemplatePartialSpecializationDecl>(spec))
        args_written = partial->getTemplateArgsAsWritten();
    else
        args_written = spec->getTemplateArgsInfo();
    if(! args_written)
        return;
    auto args = args_written->arguments();
    buildTemplateArgs(I.Args, std::views::transform(
        args, [](auto& x) -> auto& { return x.getArgument(); }));
}

void
ASTVisitor::
parseTemplateArgs(
    TemplateInfo& I,
    const FunctionTemplateSpecializationInfo* spec)
{
    // KRYSTIAN FIXME: should this use getTemplateInstantiationPattern?
    // ID of the primary template
    // KRYSTIAN NOTE: do we need to check I->Primary.has_value()?
    if(FunctionTemplateDecl* primary = spec->getTemplate())
    {
        if(auto* MT = primary->getInstantiatedFromMemberTemplate())
            primary = MT;
        extractSymbolID(primary, I.Primary.emplace());
    }
    // TemplateArguments is used instead of TemplateArgumentsAsWritten
    // because explicit specializations of function templates may have
    // template arguments deduced from their return type and parameters
    if(auto* args = spec->TemplateArguments)
        buildTemplateArgs(I.Args, args->asArray());
}

void
ASTVisitor::
parseTemplateArgs(
    TemplateInfo& I,
    const ClassScopeFunctionSpecializationDecl* spec)
{
    // if(! spec->hasExplicitTemplateArgs())
    //     return;
    // KRYSTIAN NOTE: we have no way to get the ID of the primary template;
    // it is unknown what function template this will be an explicit
    // specialization of until the enclosing class template is instantiated.
    // this also means that we can only extract the explicit template arguments.
    // in the future, we could use name lookup to find matching declarations
    if(auto* args_written = spec->getTemplateArgsAsWritten())
    {
        auto args = args_written->arguments();
        buildTemplateArgs(I.Args, std::views::transform(
            args, [](auto& x) -> auto& { return x.getArgument(); }));
    }
}

void
ASTVisitor::
parseTemplateParams(
    TemplateInfo& I,
    const Decl* D)
{
    if(const TemplateParameterList* ParamList =
        D->getDescribedTemplateParams())
    {
        for(const NamedDecl* ND : *ParamList)
        {
            I.Params.emplace_back(
                buildTemplateParam(ND));
        }
    }
}

void
ASTVisitor::
applyDecayToParameters(
    const FunctionDecl* D)
{
    // apply the type adjustments specified in [dcl.fct] p5
    // to ensure that the USR of the corresponding function matches
    // other declarations of the function that have parameters declared
    // with different top-level cv-qualifiers.
    // this needs to be done prior to USR generation for the function
    for(ParmVarDecl* P : D->parameters())
        P->setType(astContext_->getSignatureParameterType(P->getType()));
}

void
ASTVisitor::
parseRawComment(
    std::unique_ptr<Javadoc>& javadoc,
    Decl const* D)
{
    // VFALCO investigate whether we can use
    // ASTContext::getCommentForDecl instead
    RawComment* RC =
        D->getASTContext().getRawCommentForDeclNoCache(D);
    parseJavadoc(javadoc, RC, D, config_);
}

//------------------------------------------------

void
ASTVisitor::
parseEnumerators(
    EnumInfo& I,
    const EnumDecl* D)
{
    for(const EnumConstantDecl* E : D->enumerators())
    {
        auto& M = I.Members.emplace_back(
            E->getNameAsString());

        buildExprInfo(
            M.Initializer,
            E->getInitExpr(),
            E->getInitVal());

        parseRawComment(I.Members.back().javadoc, E);
    }
}

//------------------------------------------------

// This also sets IsFileInRootDir
bool
ASTVisitor::
shouldExtract(
    const Decl* D)
{
    namespace path = llvm::sys::path;

    // skip system header
    if(! forceExtract_ && sourceManager_->isInSystemHeader(D->getLocation()))
        return false;

    const PresumedLoc loc =
        sourceManager_->getPresumedLoc(D->getBeginLoc());

    auto [it, inserted] = fileFilter_.emplace(
        loc.getIncludeLoc().getRawEncoding(),
        FileFilter());

    FileFilter& ff = it->second;
    File_ = files::makePosixStyle(loc.getFilename());

    // file has not been previously visited
    if(inserted)
        ff.include = config_.shouldExtractFromFile(File_, ff.prefix);

    // don't extract if the declaration is in a file
    // that should not be visited
    if(! forceExtract_ && ! ff.include)
        return false;
    // VFALCO we could assert that the prefix
    //        matches and just lop off the
    //        first ff.prefix.size() characters.
    path::replace_path_prefix(File_, ff.prefix, "");

    IsFileInRootDir_ = true;

    return true;
}

std::string
ASTVisitor::
extractName(
    const NamedDecl* D)
{
    std::string result;
    DeclarationName N = D->getDeclName();
    switch(N.getNameKind())
    {
    case DeclarationName::Identifier:
        if(const auto* I = N.getAsIdentifierInfo())
            result.append(I->getName());
        break;
    case DeclarationName::CXXDestructorName:
        result.push_back('~');
        [[fallthrough]];
    case DeclarationName::CXXConstructorName:
        if(const auto* R = N.getCXXNameType()->getAsCXXRecordDecl())
            result.append(R->getIdentifier()->getName());
        break;
    case DeclarationName::CXXDeductionGuideName:
        if(const auto* T = N.getCXXDeductionGuideTemplate())
            result.append(T->getIdentifier()->getName());
        break;
    case DeclarationName::CXXConversionFunctionName:
    {
        MRDOX_ASSERT(isa<CXXConversionDecl>(D));
        const auto* CD = cast<CXXConversionDecl>(D);
        result.append("operator ");
        // KRYSTIAN FIXME: we *really* should not be
        // converting types to strings like this
        result.append(toString(
            *buildTypeInfo(
                CD->getReturnType())));
        break;
    }
    case DeclarationName::CXXOperatorName:
    {
        OperatorKind K = convertToOperatorKind(
            N.getCXXOverloadedOperator());
        result.append("operator");
        std::string_view name = getOperatorName(K);
        if(std::isalpha(name.front()))
            result.push_back(' ');
        result.append(name);
        break;
    }
    case DeclarationName::CXXLiteralOperatorName:
    case DeclarationName::CXXUsingDirective:
        break;
    default:
        MRDOX_UNREACHABLE();
    }
    return result;
}

//------------------------------------------------

void
ASTVisitor::
getParentNamespaces(
    Info& I,
    Decl* D)
{
    // this function should be called once per Info
    MRDOX_ASSERT(I.Namespace.empty());

    Decl* child = D;
    SymbolID child_id = I.id;
    DeclContext* parent_context = child->getDeclContext();
    do
    {
        // Decl* parent = getInstantiatedFrom(
        //     cast<Decl>(parent_context));
        // parent_context = cast<DeclContext>(parent);
        Decl* parent = cast<Decl>(parent_context);
        SymbolID parent_id = extractSymbolID(parent);
        switch(parent_context->getDeclKind())
        {
        // the TranslationUnit DeclContext is the global namespace;
        // it uses SymbolID::zero and should *always* exist
        case Decl::TranslationUnit:
        {
            auto [P, created] = getOrCreateInfo<
                NamespaceInfo>(SymbolID::zero);
            emplaceChild(P, child_id);
            break;
        }
        case Decl::Namespace:
        {
            auto [P, created] = getOrCreateInfo<
                NamespaceInfo>(parent_id);
            buildNamespace(P, created, cast<NamespaceDecl>(parent));
            emplaceChild(P, child_id);
            break;
        }
        // special case for an explicit specializations of
        // a member of an implicit instantiation.
        case Decl::ClassTemplateSpecialization:
        case Decl::ClassTemplatePartialSpecialization:
        if(auto* S = dyn_cast<ClassTemplateSpecializationDecl>(parent_context);
            S && S->getSpecializationKind() == TSK_ImplicitInstantiation)
        {
            // KRYSTIAN FIXME: i'm pretty sure DeclContext::getDeclKind()
            // will never be Decl::ClassTemplatePartialSpecialization for
            // implicit instantiations; instead, the ClassTemplatePartialSpecializationDecl
            // is accessible through S->getSpecializedTemplateOrPartial
            // if the implicit instantiation used a partially specialized template,
            MRDOX_ASSERT(parent_context->getDeclKind() !=
                Decl::ClassTemplatePartialSpecialization);

            auto [P, created] = getOrCreateInfo<
                SpecializationInfo>(parent_id);
            buildSpecialization(P, created, S);
            // KRYSTIAN FIXME: extract primary/specialized ID properly
            emplaceChild(P, SpecializedMember(child_id, child_id));
            break;
        }
        // non-implicit instantiations should be
        // treated like normal CXXRecordDecls
        [[fallthrough]];
        // we should never encounter a Record
        // that is not a CXXRecord
        case Decl::CXXRecord:
        {
            auto [P, created] = getOrCreateInfo<
                RecordInfo>(parent_id);
            buildRecord(P, created, cast<CXXRecordDecl>(parent));
            emplaceChild(P, child_id);
            break;
        }
        // KRYSTIAN FIXME: we may need to handle
        // enumerators separately at some point 
        // case Decl::Enum:
        default:
            // we consider all other DeclContexts to be "transparent"
            // and do not include them in the list of parents.
            continue;
        }
        I.Namespace.emplace_back(parent_id);
        child = parent;
        child_id = parent_id;
    }
    while((parent_context = parent_context->getParent()));
}

template<
    typename InfoTy,
    typename Child>
void
ASTVisitor::
emplaceChild(
    InfoTy& I,
    Child&& C)
{
    if constexpr(requires { I.Specializations; })
    {
        auto& S = I.Members;
        if(Info* child = getInfo(C); 
            child && child->isSpecialization())
        {
            if(std::find(S.begin(), S.end(), C) == S.end())
                S.emplace_back(C);
            return;
        }
    }
    auto& M = I.Members;
    if(std::find(M.begin(), M.end(), C) == M.end())
        M.emplace_back(C);
}

//------------------------------------------------

void
ASTVisitor::
buildSpecialization(
    SpecializationInfo& I,
    bool created,
    ClassTemplateSpecializationDecl* D)
{
    if(! created)
        return;

    const CXXRecordDecl* RD =
        D->getTemplateInstantiationPattern();
    MRDOX_ASSERT(RD);

    buildTemplateArgs(I.Args,
        D->getTemplateArgs().asArray());

    extractSymbolID(RD, I.Primary);
    I.Name = extractName(RD);

    getParentNamespaces(I, D);
}

//------------------------------------------------
// Decl types which have isThisDeclarationADefinition:
//
// VarTemplateDecl
// FunctionTemplateDecl
// FunctionDecl
// TagDecl
// ClassTemplateDecl
// CXXDeductionGuideDecl

void
ASTVisitor::
buildNamespace(
    NamespaceInfo& I,
    bool created,
    NamespaceDecl* D)
{
    if(! created)
        return;

    // KRYSTIAN NOTE: we do not extract
    // javadocs for namespaces
    if(D->isAnonymousNamespace())
        I.specs.isAnonymous = true;
    else
        I.Name = extractName(D);
    I.specs.isInline = D->isInline();

    getParentNamespaces(I, D);
}

//------------------------------------------------

void
ASTVisitor::
buildRecord(
    RecordInfo& I,
    bool created,
    CXXRecordDecl* D)
{
    parseRawComment(I.javadoc, D);
    addSourceLocation(I, getLine(D),
        D->isThisDeclarationADefinition());

    if(! created)
        return;

    NamedDecl* ND = D;
    if(TypedefNameDecl* TD =
        D->getTypedefNameForAnonDecl())
    {
        I.IsTypeDef = true;
        ND = TD;
    }
    I.Name = extractName(ND);

    I.KeyKind = convertToRecordKeyKind(D->getTagKind());

    // These are from CXXRecordDecl::isEffectivelyFinal()
    I.specs.isFinal = D->template hasAttr<FinalAttr>();
    if(const auto* DT = D->getDestructor())
        I.specs.isFinalDestructor = DT->template hasAttr<FinalAttr>();

    // extract direct bases. D->bases() will get the bases
    // from whichever declaration is the definition (if any)
    if(D->hasDefinition())
    {
        for(const CXXBaseSpecifier& B : D->bases())
        {
            I.Bases.emplace_back(
                buildTypeInfo(B.getType()),
                convertToAccessKind(
                    B.getAccessSpecifier()),
                B.isVirtual());
        }
    }

    getParentNamespaces(I, D);
}

//------------------------------------------------

void
ASTVisitor::
buildEnum(
    EnumInfo& I,
    bool created,
    EnumDecl* D)
{
    parseRawComment(I.javadoc, D);
    addSourceLocation(I, getLine(D),
        D->isThisDeclarationADefinition());

    if(! created)
        return;

    I.Name = extractName(D);

    I.Scoped = D->isScoped();

    if(D->isFixed())
        I.UnderlyingType = buildTypeInfo(
            D->getIntegerType());

    parseEnumerators(I, D);

    getParentNamespaces(I, D);
}

//------------------------------------------------

void
ASTVisitor::
buildTypedef(
    TypedefInfo& I,
    bool created,
    TypedefNameDecl* D)
{
    parseRawComment(I.javadoc, D);
    // KRYSTIAN FIXME: we currently treat typedef/alias
    // declarations as having a single definition; however,
    // such declarations are never definitions and can
    // be redeclared multiple times (even in the same scope)
    addSourceLocation(I, getLine(D), true);

    if(! created)
        return;

    I.Name = extractName(D);

    I.Type = buildTypeInfo(
        D->getUnderlyingType());

#if 0
    if(I.Type.Name.empty())
    {
        // Typedef for an unnamed type. This is like
        // "typedef struct { } Foo;". The record serializer
        // explicitly checks for this syntax and constructs
        // a record with that name, so we don't want to emit
        // a duplicate here.
        return;
    }
#endif

    getParentNamespaces(I, D);
}

//------------------------------------------------

void
ASTVisitor::
buildVariable(
    VariableInfo& I,
    bool created,
    VarDecl* D)
{
    parseRawComment(I.javadoc, D);
    addSourceLocation(I, getLine(D),
        D->isThisDeclarationADefinition());

    // KRYSTIAN FIXME: we need to properly merge storage class
    I.specs.storageClass |=
        convertToStorageClassKind(
            D->getStorageClass());

    // this handles thread_local, as well as the C
    // __thread and __Thread_local specifiers
    I.specs.isThreadLocal |= D->getTSCSpec() !=
        ThreadStorageClassSpecifier::TSCS_unspecified;

    // KRYSTIAN NOTE: VarDecl does not provide getConstexprKind,
    // nor does it use getConstexprKind to store whether
    // a variable is constexpr/constinit. Although
    // only one is permitted in a variable declaration,
    // it is possible to declare a static data member
    // as both constexpr and constinit in separate declarations..
    I.specs.isConstinit |= D->hasAttr<ConstInitAttr>();
    if(D->isConstexpr())
        I.specs.constexprKind = ConstexprKind::Constexpr;

    if(! created)
        return;

    I.Name = extractName(D);

    I.Type = buildTypeInfo(D->getType());

    getParentNamespaces(I, D);
}

//------------------------------------------------

void
ASTVisitor::
buildField(
    FieldInfo& I,
    bool created,
    FieldDecl* D)
{
    parseRawComment(I.javadoc, D);
    // fields (i.e. non-static data members)
    // cannot have multiple declarations
    addSourceLocation(I, getLine(D), true);

    if(! created)
        return;

    I.Name = extractName(D);

    I.Type = buildTypeInfo(D->getType());

    I.IsMutable = D->isMutable();

    if(D->isBitField())
    {
        I.IsBitfield = true;
        buildExprInfo(
            I.BitfieldWidth,
            D->getBitWidth());
    }

    I.specs.hasNoUniqueAddress = D->hasAttr<NoUniqueAddressAttr>();
    I.specs.isDeprecated = D->hasAttr<DeprecatedAttr>();
    I.specs.isMaybeUnused = D->hasAttr<UnusedAttr>();

    getParentNamespaces(I, D);
}

//------------------------------------------------

template<class DeclTy>
void
ASTVisitor::
buildFunction(
    FunctionInfo& I,
    bool created,
    DeclTy* D)
{
    parseRawComment(I.javadoc, D);
    addSourceLocation(I, getLine(D),
        D->isThisDeclarationADefinition());

    //
    // FunctionDecl
    //
    I.specs0.isVariadic |= D->isVariadic();
    I.specs0.isDefaulted |= D->isDefaulted();
    I.specs0.isExplicitlyDefaulted |= D->isExplicitlyDefaulted();
    I.specs0.isDeleted |= D->isDeleted();
    I.specs0.isDeletedAsWritten |= D->isDeletedAsWritten();
    I.specs0.isNoReturn |= D->isNoReturn();
        // subsumes D->hasAttr<NoReturnAttr>()
        // subsumes D->hasAttr<CXX11NoReturnAttr>()
        // subsumes D->hasAttr<C11NoReturnAttr>()
        // subsumes D->getType()->getAs<FunctionType>()->getNoReturnAttr()
    I.specs0.hasOverrideAttr |= D->template hasAttr<OverrideAttr>();
    if(auto const* FP = D->getType()->template getAs<FunctionProtoType>())
        I.specs0.hasTrailingReturn |= FP->hasTrailingReturn();
    I.specs0.constexprKind |=
        convertToConstexprKind(
            D->getConstexprKind());
    I.specs0.exceptionSpec |=
        convertToNoexceptKind(
            D->getExceptionSpecType());
    I.specs0.overloadedOperator |=
        convertToOperatorKind(
            D->getOverloadedOperator());
    I.specs0.storageClass |=
        convertToStorageClassKind(
            D->getStorageClass());

    I.specs1.isNodiscard |= D->template hasAttr<WarnUnusedResultAttr>();

    //
    // CXXMethodDecl
    //
    if constexpr(std::derived_from<DeclTy, CXXMethodDecl>)
    {
        I.specs0.isVirtual |= D->isVirtual();
        I.specs0.isVirtualAsWritten |= D->isVirtualAsWritten();
        I.specs0.isPure |= D->isPure();
        I.specs0.isConst |= D->isConst();
        I.specs0.isVolatile |= D->isVolatile();
        I.specs0.refQualifier |=
            convertToReferenceKind(
                D->getRefQualifier());
        I.specs0.isFinal |= D->template hasAttr<FinalAttr>();
        //D->isCopyAssignmentOperator()
        //D->isMoveAssignmentOperator()
        //D->isOverloadedOperator();
        //D->isStaticOverloadedOperator();
    }

    //
    // CXXDestructorDecl
    //
    if constexpr(std::derived_from<DeclTy, CXXDestructorDecl>)
    {
    }

    //
    // CXXConstructorDecl
    //
    if constexpr(std::derived_from<DeclTy, CXXConstructorDecl>)
    {
        I.specs1.explicitSpec |=
            convertToExplicitKind(
                D->getExplicitSpecifier());
    }

    //
    // CXXConversionDecl
    //
    if constexpr(std::derived_from<DeclTy, CXXConversionDecl>)
    {
        I.specs1.explicitSpec |=
            convertToExplicitKind(
                D->getExplicitSpecifier());
    }

    //
    // CXXDeductionGuideDecl
    //
    if constexpr(std::derived_from<DeclTy, CXXDeductionGuideDecl>)
    {
        I.specs1.explicitSpec |=
            convertToExplicitKind(
                D->getExplicitSpecifier());
    }

    if(! created)
        return;

    I.Name = extractName(D);

    for(const ParmVarDecl* P : D->parameters())
    {
        I.Params.emplace_back(
            buildTypeInfo(P->getOriginalType()),
            P->getNameAsString(),
            getSourceCode(P->getDefaultArgRange()));
    }

    I.ReturnType = buildTypeInfo(
        D->getReturnType());

    if(const auto* ftsi = D->getTemplateSpecializationInfo())
    {
        if(! I.Template)
            I.Template = std::make_unique<TemplateInfo>();
        parseTemplateArgs(*I.Template, ftsi);
    }

    getParentNamespaces(I, D);
}

//------------------------------------------------

void
ASTVisitor::
buildFriend(
    FriendDecl* D)
{
    if(NamedDecl* ND = D->getFriendDecl())
    {
        // D does not name a type
        if(FunctionDecl* FD = dyn_cast<FunctionDecl>(ND))
        {
            if(! shouldExtract(FD))
                return;

            SymbolID id;
            if(! extractSymbolID(FD, id))
                return;
            auto [I, created] = getOrCreateInfo<FunctionInfo>(id);
            buildFunction(I, created, FD);

            const DeclContext* DC = D->getDeclContext();
            const auto* RD = dyn_cast<CXXRecordDecl>(DC);
            // the semantic DeclContext of a FriendDecl must be a class
            MRDOX_ASSERT(RD);
            SymbolID parent_id = extractSymbolID(RD);
            if(Info* parent = getInfo(parent_id))
            {
                MRDOX_ASSERT(parent->isRecord());
                static_cast<RecordInfo*>(parent)->Friends.emplace_back(I.id);
            }

            return;
        }
        if(FunctionTemplateDecl* FT = dyn_cast<FunctionTemplateDecl>(ND))
        {
            // VFALCO TODO
            (void)FT;
            return;
        }
        if(ClassTemplateDecl* CT = dyn_cast<ClassTemplateDecl>(ND))
        {
            // VFALCO TODO
            (void)CT;
            return;
        }

        MRDOX_UNREACHABLE();
    }
    else if(TypeSourceInfo* TS = D->getFriendType())
    {
        (void)TS;
        return;
    }
    else
    {
        MRDOX_UNREACHABLE();
    }
    return;
}

//------------------------------------------------

bool
ASTVisitor::
traverse(NamespaceDecl* D)
{
    if(! shouldExtract(D))
        return true;
    if(! config_.includeAnonymous &&
        D->isAnonymousNamespace())
        return true;

    SymbolID id;
    if(! extractSymbolID(D, id))
        return true;
    auto [I, created] = getOrCreateInfo<NamespaceInfo>(id);

    buildNamespace(I, created, D);
    return traverseContext(D);
}

bool
ASTVisitor::
traverse(CXXRecordDecl* D,
    AccessSpecifier A,
    std::unique_ptr<TemplateInfo>&& Template = nullptr)
{
    if(! shouldExtract(D))
        return true;

    MRDOX_ASSERT(! D->getDeclContext()->isRecord() ||
        A != AccessSpecifier::AS_none);

    SymbolID id;
    if(! extractSymbolID(D, id))
        return false;
    auto [I, created] = getOrCreateInfo<RecordInfo>(id);
    I.Access = convertToAccessKind(A);
    I.Template = std::move(Template);

    buildRecord(I, created, D);
    return traverseContext(D);
}

bool
ASTVisitor::
traverse(EnumDecl* D,
    AccessSpecifier A)
{
    if(! shouldExtract(D))
        return true;

    MRDOX_ASSERT(! D->getDeclContext()->isRecord() ||
        A != AccessSpecifier::AS_none);

    SymbolID id;
    if(! extractSymbolID(D, id))
        return false;
    auto [I, created] = getOrCreateInfo<EnumInfo>(id);
    I.Access = convertToAccessKind(A);

    buildEnum(I, created, D);
    return true;
}

bool
ASTVisitor::
traverse(TypedefDecl* D,
    AccessSpecifier A)
{
    if(! shouldExtract(D))
        return true;

    MRDOX_ASSERT(! D->getDeclContext()->isRecord() ||
        A != AccessSpecifier::AS_none);

    SymbolID id;
    if(! extractSymbolID(D, id))
        return false;
    auto [I, created] = getOrCreateInfo<TypedefInfo>(id);
    I.Access = convertToAccessKind(A);

    buildTypedef(I, created, D);
    return true;
}

bool
ASTVisitor::
traverse(TypeAliasDecl* D,
    AccessSpecifier A,
    std::unique_ptr<TemplateInfo>&& Template = nullptr)
{
    if(! shouldExtract(D))
        return true;

    MRDOX_ASSERT(! D->getDeclContext()->isRecord() ||
        A != AccessSpecifier::AS_none);

    SymbolID id;
    if(! extractSymbolID(D, id))
        return false;
    auto [I, created] = getOrCreateInfo<TypedefInfo>(id);
    I.Access = convertToAccessKind(A);
    I.IsUsing = true;
    I.Template = std::move(Template);

    buildTypedef(I, created, D);
    return true;
}

bool
ASTVisitor::
traverse(VarDecl* D,
    AccessSpecifier A,
    std::unique_ptr<TemplateInfo>&& Template = nullptr)
{
    if(! shouldExtract(D))
        return true;

    MRDOX_ASSERT(! D->getDeclContext()->isRecord() ||
        A != AccessSpecifier::AS_none);

    SymbolID id;
    if(! extractSymbolID(D, id))
        return false;
    auto [I, created] = getOrCreateInfo<VariableInfo>(id);
    I.Access = convertToAccessKind(A);
    I.Template = std::move(Template);

    buildVariable(I, created, D);
    return true;
}

bool
ASTVisitor::
traverse(FieldDecl* D,
    AccessSpecifier A)
{
    if(! shouldExtract(D))
        return true;

    MRDOX_ASSERT(D->getDeclContext()->isRecord());
    MRDOX_ASSERT(A != AccessSpecifier::AS_none);

    SymbolID id;
    if(! extractSymbolID(D, id))
        return false;
    auto [I, created] = getOrCreateInfo<FieldInfo>(id);
    I.Access = convertToAccessKind(A);

    buildField(I, created, D);
    return true;
}

bool
ASTVisitor::
traverse(FunctionDecl* D,
    AccessSpecifier A,
    std::unique_ptr<TemplateInfo>&& Template = nullptr)
{
    if(! shouldExtract(D))
        return true;

    MRDOX_ASSERT(! D->getDeclContext()->isRecord());
    MRDOX_ASSERT(A == AccessSpecifier::AS_none);

    SymbolID id;
    if(! extractSymbolID(D, id))
        return false;
    auto [I, created] = getOrCreateInfo<FunctionInfo>(id);
    I.Access = convertToAccessKind(A);
    I.Template = std::move(Template);

    buildFunction(I, created, D);
    return true;
}

bool
ASTVisitor::
traverse(CXXMethodDecl* D,
    AccessSpecifier A,
    std::unique_ptr<TemplateInfo>&& Template = nullptr)
{
    if(! shouldExtract(D))
        return true;

    MRDOX_ASSERT(D->getDeclContext()->isRecord());
    MRDOX_ASSERT(A != AccessSpecifier::AS_none);

    SymbolID id;
    if(! extractSymbolID(D, id))
        return false;
    auto [I, created] = getOrCreateInfo<FunctionInfo>(id);
    I.Access = convertToAccessKind(A);
    I.Template = std::move(Template);

    buildFunction(I, created, D);
    return true;
}

bool
ASTVisitor::
traverse(CXXConstructorDecl* D,
    AccessSpecifier A,
    std::unique_ptr<TemplateInfo>&& Template = nullptr)
{
    if(! shouldExtract(D))
        return true;

    MRDOX_ASSERT(D->getDeclContext()->isRecord());
    MRDOX_ASSERT(A != AccessSpecifier::AS_none);

    SymbolID id;
    if(! extractSymbolID(D, id))
        return false;
    auto [I, created] = getOrCreateInfo<FunctionInfo>(id);
    I.Access = convertToAccessKind(A);
    I.Template = std::move(Template);
    I.Class = FunctionClass::Constructor;

    buildFunction(I, created, D);
    return true;
}

bool
ASTVisitor::
traverse(CXXConversionDecl* D,
    AccessSpecifier A,
    std::unique_ptr<TemplateInfo>&& Template = nullptr)
{
    if(! shouldExtract(D))
        return true;

    MRDOX_ASSERT(D->getDeclContext()->isRecord());
    MRDOX_ASSERT(A != AccessSpecifier::AS_none);

    SymbolID id;
    if(! extractSymbolID(D, id))
        return false;
    auto [I, created] = getOrCreateInfo<FunctionInfo>(id);
    I.Access = convertToAccessKind(A);
    I.Template = std::move(Template);
    I.Class = FunctionClass::Conversion;

    buildFunction(I, created, D);
    return true;
}

bool
ASTVisitor::
traverse(CXXDeductionGuideDecl* D,
    AccessSpecifier A,
    std::unique_ptr<TemplateInfo>&& Template = nullptr)
{
    if(! shouldExtract(D))
        return true;

    SymbolID id;
    if(! extractSymbolID(D, id))
        return false;
    auto [I, created] = getOrCreateInfo<FunctionInfo>(id);
    I.Access = convertToAccessKind(A);
    I.Template = std::move(Template);
    I.Class = FunctionClass::Deduction;

    buildFunction(I, created, D);
    return true;
}

bool
ASTVisitor::
traverse(CXXDestructorDecl* D,
    AccessSpecifier A)
{
    if(! shouldExtract(D))
        return true;

    MRDOX_ASSERT(D->getDeclContext()->isRecord());
    MRDOX_ASSERT(A != AccessSpecifier::AS_none);

    SymbolID id;
    if(! extractSymbolID(D, id))
        return false;
    auto [I, created] = getOrCreateInfo<FunctionInfo>(id);
    I.Access = convertToAccessKind(A);
    I.Class = FunctionClass::Destructor;

    buildFunction(I, created, D);
    return true;
}

bool
ASTVisitor::
traverse(FriendDecl* D)
{
    buildFriend(D);
    return true;
}

//------------------------------------------------

bool
ASTVisitor::
traverse(ClassTemplateDecl* D,
    AccessSpecifier A)
{
    CXXRecordDecl* RD = D->getTemplatedDecl();
    if(! shouldExtract(RD))
        return true;

    auto Template = std::make_unique<TemplateInfo>();
    parseTemplateParams(*Template, RD);

    return traverse(RD, A, std::move(Template));
}

bool
ASTVisitor::
traverse(ClassTemplateSpecializationDecl* D)
{
    CXXRecordDecl* RD = D;
    if(! shouldExtract(RD))
        return true;

    auto Template = std::make_unique<TemplateInfo>();
    parseTemplateParams(*Template, RD);
    parseTemplateArgs(*Template, D);

    // determine the access from the primary template
    return traverse(RD,
        D->getSpecializedTemplate()->getAccessUnsafe(),
        std::move(Template));
}

bool
ASTVisitor::
traverse(VarTemplateDecl* D,
    AccessSpecifier A)
{
    VarDecl* VD = D->getTemplatedDecl();
    if(! shouldExtract(VD))
        return true;

    auto Template = std::make_unique<TemplateInfo>();
    parseTemplateParams(*Template, VD);

    return traverse(VD, A, std::move(Template));
}

bool
ASTVisitor::
traverse(VarTemplateSpecializationDecl* D)
{
    VarDecl* VD = D;
    if(! shouldExtract(VD))
        return true;

    auto Template = std::make_unique<TemplateInfo>();
    parseTemplateParams(*Template, VD);
    parseTemplateArgs(*Template, D);

    return traverse(VD,
        D->getSpecializedTemplate()->getAccessUnsafe(),
        std::move(Template));
}

bool
ASTVisitor::
traverse(FunctionTemplateDecl* D,
    AccessSpecifier A)
{
    FunctionDecl* FD = D->getTemplatedDecl();
    // check whether to extract using the templated declaration.
    // this is done because the template-head may be implicit
    // (e.g. for an abbreviated function template with no template-head)
    if(! shouldExtract(FD))
        return true;
    auto Template = std::make_unique<TemplateInfo>();
    parseTemplateParams(*Template, FD);

    // traverse the templated declaration according to its kind
    return traverseDecl(FD, std::move(Template));
}

bool
ASTVisitor::
traverse(ClassScopeFunctionSpecializationDecl* D)
{
    if(! shouldExtract(D))
        return true;

    /* For class scope explicit specializations of member function templates which
       are members of class templates, it is impossible to know what the
       primary template is until the enclosing class template is instantiated.
       while such declarations are valid C++ (see CWG 727 and [temp.expl.spec] p3),
       GCC does not consider them to be valid. Consequently, we do not extract the SymbolID
       of the primary template. In the future, we could take a best-effort approach to find
       the primary template, but this is only possible when none of the candidates are dependent
       upon a template parameter of the enclosing class template.
    */
    auto Template = std::make_unique<TemplateInfo>();
    parseTemplateArgs(*Template, D);

    CXXMethodDecl* MD = D->getSpecialization();

    // since the templated CXXMethodDecl may be a constructor
    // or conversion function, call TraverseDecl to ensure that
    // we call traverse for the dynamic type of the CXXMethodDecl
    return traverseDecl(MD, std::move(Template));
}

bool
ASTVisitor::
traverse(TypeAliasTemplateDecl* D,
    AccessSpecifier A)
{
    TypeAliasDecl* AD = D->getTemplatedDecl();
    if(! shouldExtract(AD))
        return true;

    auto Template = std::make_unique<TemplateInfo>();
    parseTemplateParams(*Template, AD);

    return traverse(AD, A, std::move(Template));
}

template<typename... Args>
auto
ASTVisitor::
traverse(Args&&...)
{
    // no matching Traverse overload found
    MRDOX_UNREACHABLE();
}

//------------------------------------------------

template<typename... Args>
bool
ASTVisitor::
traverseDecl(
    Decl* D,
    Args&&... args)
{
    MRDOX_ASSERT(D);
    if(D->isInvalidDecl() || D->isImplicit())
        return true;

    AccessSpecifier access =
        D->getAccessUnsafe();

    switch(D->getKind())
    {
    case Decl::Namespace:
        traverse(static_cast<
            NamespaceDecl*>(D),
            std::forward<Args>(args)...);
        break;
    case Decl::CXXRecord:
        traverse(static_cast<
            CXXRecordDecl*>(D),
            access,
            std::forward<Args>(args)...);
        break;
    case Decl::CXXMethod:
        traverse(static_cast<
            CXXMethodDecl*>(D),
            access,
            std::forward<Args>(args)...);
        break;
    case Decl::CXXConstructor:
        traverse(static_cast<
            CXXConstructorDecl*>(D),
            access,
            std::forward<Args>(args)...);
        break;
    case Decl::CXXConversion:
        traverse(static_cast<
            CXXConversionDecl*>(D),
            access,
            std::forward<Args>(args)...);
        break;
    case Decl::CXXDestructor:
        traverse(static_cast<
            CXXDestructorDecl*>(D),
            access,
            std::forward<Args>(args)...);
        break;
    case Decl::CXXDeductionGuide:
        traverse(static_cast<
            CXXDeductionGuideDecl*>(D),
            access,
            std::forward<Args>(args)...);
        break;
    case Decl::Function:
        traverse(static_cast<
            FunctionDecl*>(D),
            access,
            std::forward<Args>(args)...);
        break;
    case Decl::Friend:
        traverse(static_cast<
            FriendDecl*>(D),
            std::forward<Args>(args)...);
        break;
    case Decl::TypeAlias:
        traverse(static_cast<
            TypeAliasDecl*>(D),
            access,
            std::forward<Args>(args)...);
        break;
    case Decl::Typedef:
        traverse(static_cast<
            TypedefDecl*>(D),
            access,
            std::forward<Args>(args)...);
        break;
    case Decl::Enum:
        traverse(static_cast<
            EnumDecl*>(D),
            access,
            std::forward<Args>(args)...);
        break;
    case Decl::Field:
        traverse(static_cast<
            FieldDecl*>(D),
            access,
            std::forward<Args>(args)...);
        break;
    case Decl::Var:
        traverse(static_cast<
            VarDecl*>(D),
            access,
            std::forward<Args>(args)...);
        break;
    case Decl::ClassTemplate:
        traverse(static_cast<
            ClassTemplateDecl*>(D),
            access,
            std::forward<Args>(args)...);
        break;
    case Decl::ClassTemplatePartialSpecialization:
    case Decl::ClassTemplateSpecialization:
        traverse(static_cast<
            ClassTemplateSpecializationDecl*>(D),
            std::forward<Args>(args)...);
        break;
    case Decl::VarTemplate:
        traverse(static_cast<
            VarTemplateDecl*>(D),
            access,
            std::forward<Args>(args)...);
        break;
    case Decl::VarTemplatePartialSpecialization:
    case Decl::VarTemplateSpecialization:
        traverse(static_cast<
            VarTemplateSpecializationDecl*>(D),
            std::forward<Args>(args)...);
        break;
    case Decl::FunctionTemplate:
        traverse(static_cast<
            FunctionTemplateDecl*>(D),
            access,
            std::forward<Args>(args)...);
        break;
    case Decl::ClassScopeFunctionSpecialization:
        traverse(static_cast<
            ClassScopeFunctionSpecializationDecl*>(D),
            std::forward<Args>(args)...);
        break;
    case Decl::TypeAliasTemplate:
        traverse(static_cast<
            TypeAliasTemplateDecl*>(D),
            access,
            std::forward<Args>(args)...);
        break;
    default:
        // for declarations we don't explicitly handle, traverse the children
        // if it has any (e.g. LinkageSpecDecl, ExportDecl, ExternCContextDecl).
        if(auto* DC = dyn_cast<DeclContext>(D))
            traverseContext(DC);
        break;
    }

    return true;
}

bool
ASTVisitor::
traverseContext(
    DeclContext* D)
{
    MRDOX_ASSERT(D);
    for(auto* C : D->decls())
        if(! traverseDecl(C))
            return false;
    return true;
}

void
ASTVisitor::
Initialize(ASTContext& Context)
{
    astContext_ = &Context;
    sourceManager_ = &Context.getSourceManager();
}

void
ASTVisitor::
InitializeSema(Sema& S)
{
    // Sema should not have been initialized yet
    MRDOX_ASSERT(! sema_);
    sema_ = &S;
}

void
ASTVisitor::
ForgetSema()
{
    sema_ = nullptr;
}

void
ASTVisitor::
HandleCXXStaticMemberVarInstantiation(VarDecl* D)
{
    // implicitly instantiated definitions of non-inline
    // static data members of class templates are added to
    // the end of the TU DeclContext. Decl::isImplicit returns
    // false for these VarDecls, so we manually set it here.
    D->setImplicit();
}

void
ASTVisitor::
HandleCXXImplicitFunctionInstantiation(FunctionDecl* D)
{
    D->setImplicit();
}

// An instance of Visitor runs on one translation unit.
void
ASTVisitor::
HandleTranslationUnit(
    ASTContext& Context)
{
    // the ASTContext and Sema better be the same
    // as those set by Initialize and InitializeSema
    MRDOX_ASSERT(astContext_ == &Context);
    MRDOX_ASSERT(sema_);

    // Install handlers for our custom commands
    initCustomCommentCommands(Context);

    std::optional<llvm::StringRef> filePath =
        Context.getSourceManager().getNonBuiltinFilenameForID(
            Context.getSourceManager().getMainFileID());
    if(! filePath)
        return;

    // Filter out TUs we don't care about
    File_ = *filePath;
    convert_to_slash(File_);
    if(! config_.shouldVisitTU(File_))
        return;

    TranslationUnitDecl* TU =
        Context.getTranslationUnitDecl();
    // the traversal scope should *only* consist of the
    // top-level TranslationUnitDecl. if this assert fires,
    // then it means ASTContext::setTraversalScope is being
    // (erroneously) used somewhere
    MRDOX_ASSERT(Context.getTraversalScope() ==
        std::vector<Decl*>{TU});

    // traverse the translation unit
    traverseContext(TU);

    // VFALCO If we returned from the function early
    // then this line won't execute, which means we
    // will miss error and warnings emitted before
    // the return.
    ex_.report(std::move(diags_));

    for(auto& info : info_)
        insertBitcode(ex_, writeBitcode(*info));
}

} // mrdox
} // clang
