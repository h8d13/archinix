#include "nix/store/export-import.hh"
#include "nix/util/serialise.hh"
#include "nix/store/store-api.hh"
#include "nix/util/archive.hh"
#include "nix/store/common-protocol.hh"
#include "nix/store/common-protocol-impl.hh"

#include <algorithm>

namespace nix {

static void exportPath(Store & store, const StorePath & path, Sink & sink)
{
    auto info = store.queryPathInfo(path);

    HashSink hashSink(HashAlgorithm::SHA256);
    TeeSink teeSink(sink, hashSink);

    store.narFromPath(path, teeSink);

    /* Refuse to export paths that have changed.  This prevents
       filesystem corruption from spreading to other machines.
       Don't complain if the stored hash is zero (unknown). */
    Hash hash = hashSink.currentHash().hash;
    if (hash != info->narHash && info->narHash != Hash(info->narHash.algo))
        throw Error(
            "hash of path '%s' has changed from '%s' to '%s'!",
            store.printStorePath(path),
            info->narHash.to_string(HashFormat::Nix32, true),
            hash.to_string(HashFormat::Nix32, true));

    teeSink << exportMagic << store.printStorePath(path);
    CommonProto::write(store, CommonProto::WriteConn{.to = teeSink}, info->references);
    teeSink << (info->deriver ? store.printStorePath(*info->deriver) : "") << 0;
}

void exportPaths(Store & store, const StorePathSet & paths, Sink & sink)
{
    auto sorted = store.topoSortPaths(paths);

    for (auto & path : sorted | std::views::reverse) {
        sink << 1;
        exportPath(store, path, sink);
    }

    sink << 0;
}

} // namespace nix
