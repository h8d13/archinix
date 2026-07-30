#include "nix/store/path-info.hh"
#include "nix/store/store-api.hh"

namespace nix {

UnkeyedValidPathInfo::UnkeyedValidPathInfo(const StoreDirConfig & store, Hash narHash)
    : UnkeyedValidPathInfo{store.storeDir, narHash}
{
}

bool ValidPathInfo::isContentAddressed(const StoreDirConfig & store) const
{
    if (!ca)
        return false;

    bool res = store.makeContentAddressedPath(path.name(), ca->hash) == path;

    if (!res)
        printError("warning: path '%s' claims to be content-addressed but isn't", store.printStorePath(path));

    return res;
}

ValidPathInfo ValidPathInfo::makeFromCA(const StoreDirConfig & store, std::string_view name, Hash narHash)
{
    ValidPathInfo res{
        store.makeContentAddressedPath(name, narHash),
        UnkeyedValidPathInfo(store, narHash),
    };
    res.ca = ContentAddress{.hash = narHash};
    return res;
}

} // namespace nix
