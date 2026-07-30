#include "nix/store/export-import.hh"
#include "nix/util/serialise.hh"
#include "nix/store/store-api.hh"
#include "nix/util/archive.hh"
#include "nix/store/common-protocol.hh"
#include "nix/store/common-protocol-impl.hh"


namespace nix {

static void exportPath(LocalStore & store, const StorePath & path, Sink & sink)
{
    auto info = store.queryPathInfo(path);

    HashSink hashSink;
    TeeSink teeSink(sink, hashSink);

    store.narFromPath(path, teeSink);

    /* Refuse to export paths that have changed.  This prevents
       filesystem corruption from spreading to other machines.
       Don't complain if the stored hash is zero (unknown). */
    Hash hash = hashSink.currentHash().hash;
    if (hash != info->narHash && info->narHash != Hash{})
        throw Error(
            "hash of path '%s' has changed from '%s' to '%s'!",
            store.printStorePath(path),
            info->narHash.to_string(HashFormat::Nix32, true),
            hash.to_string(HashFormat::Nix32, true));

    /* NAR, magic, path, references. The deriver name and the
       signature marker that used to follow are gone: nothing here
       builds paths and nothing signs them, so both were a constant
       empty field on every path this store has ever exported. The
       references field stays because it is what import-path checks. */
    teeSink << exportMagic << store.printStorePath(path);
    CommonProto::write(store, CommonProto::WriteConn{.to = teeSink}, info->references);
}

/* No topological sort on the way out: the paths are reference-free, so
   there is no order the receiving side needs them in. */
void exportPaths(LocalStore & store, const StorePathSet & paths, Sink & sink)
{
    for (auto & path : paths) {
        sink << 1;
        exportPath(store, path, sink);
    }

    sink << 0;
}

} // namespace nix
