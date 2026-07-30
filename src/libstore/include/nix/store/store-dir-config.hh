#pragma once

#include "nix/store/path.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/hash.hh"
#include "nix/store/content-address.hh"

#include <map>
#include <string>
#include <variant>

namespace nix {

struct SourcePath;

MakeError(BadStorePath, Error);
MakeError(BadStorePathName, BadStorePath);

/**
 * @todo This should just be inherited by `StoreConfig`. However, it
 * would be a huge amount of churn if `Store` didn't have these methods
 * anymore, forcing a bunch of code to go from `store.method(...)` to
 * `store.config.method(...)`.
 *
 * @todo this should not have "config" in its name, because it no longer
 * uses the configuration system for `storeDir` --- in fact, `storeDir`
 * isn't even owned, but a mere reference. But doing that rename would
 * cause a bunch of churn.
 */
struct StoreDirConfig
{
    const std::string & storeDir;

    // pure methods

    StorePath parseStorePath(std::string_view path) const;

    std::optional<StorePath> maybeParseStorePath(std::string_view path) const;

    std::string printStorePath(const StorePath & path) const;

    /**
     * @return true if *path* is in the Nix store (but not the Nix
     * store itself).
     */
    bool isInStore(std::string_view path) const;

    /**
     * Split a path like `/nix/store/<hash>-<name>/<bla>` into
     * `/nix/store/<hash>-<name>` and `/<bla>`.
     */
    std::pair<StorePath, CanonPath> toStorePath(std::string_view path) const;

    /**
     * Constructs a unique store path name.
     */
    StorePath makeStorePath(std::string_view type, std::string_view hash, std::string_view name) const;
    StorePath makeStorePath(std::string_view type, const Hash & hash, std::string_view name) const;

    /**
     * Where a NAR with this SHA-256 hash and name lands. One layout,
     * because there is one way in: upstream's `output:out` digest
     * (non-SHA256 or flat ingestion) and `text:` variants had no
     * caller left, and references, which used to be stuffed into the
     * type string, are refused at the import door.
     */
    StorePath makeContentAddressedPath(std::string_view name, const Hash & narHash) const;
};

} // namespace nix
