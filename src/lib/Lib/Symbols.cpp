
#include "Symbols.hpp"
#include <string_view>

namespace clang {
namespace mrdocs {

#if 0
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

SymbolIDRegistry::
SymbolIDRegistry(const InfoContext& context)
    : context_(context)
    , global_namespace_(context, []()
        {
            RawID global_id;
            global_id.fill(0xFF);
            return global_id;
        }())
{
}

SymbolID
SymbolIDRegistry::
get(const RawID& id)
{
    // don't access the set if the ID is invalid
    // is the or the ID of the global namespace
    if(id == RawID())
        return SymbolID();
    if(id == global_namespace_.data_)
        return global_namespace_.toSymbolID();

    // see if the ID already exists
    std::shared_lock<std::shared_mutex> read_lock(mutex_);
    // if the ID already exists, return the SymbolID for that entry
    if(auto it = symbols_.find(id);
        it != symbols_.end())
        return it->get()->toSymbolID();

    // otherwise, insert the new entry
    std::unique_lock<std::shared_mutex> write_lock(mutex_);
    auto [it, created] = symbols_.emplace(
        std::make_unique<SymbolIDImpl()>());
    return it->get()->toSymbolID();
}
#endif

} // mrdocs
} // clang
