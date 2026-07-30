#include "nix/util/source-path.hh"
#include "nix/util/util.hh"
#include "nix/store/store-dir-config.hh"
#include "nix/store/globals.hh"

namespace nix {

StorePath StoreDirConfig::parseStorePath(std::string_view path) const
{
    if (path.empty())
        throw BadStorePath("empty path is not a valid store path");
    auto p =
        canonPath(std::string(path))
        ;
    if (p.parent_path() != storeDir)
        throw BadStorePath("path %s is not in the Nix store", PathFmt(p));
    return StorePath(p.filename().string());
}

std::optional<StorePath> StoreDirConfig::maybeParseStorePath(std::string_view path) const
{
    try {
        return parseStorePath(path);
    } catch (Error &) {
        return {};
    }
}

std::string StoreDirConfig::printStorePath(const StorePath & path) const
{
    return (storeDir + "/").append(path.to_string());
}

/*
The exact specification of store paths is in `protocols/store-path.md`
in the Nix manual. These few functions implement that specification.

If changes to these functions go beyond mere implementation changes i.e.
also update the user-visible behavior, please update the specification
to match.
*/

StorePath StoreDirConfig::makeStorePath(std::string_view type, std::string_view hash, std::string_view name) const
{
    /* e.g., "source:sha256:1abc...:/nix/store:foo.tar.gz" */
    auto s = std::string(type) + ":" + std::string(hash) + ":" + storeDir + ":" + std::string(name);
    auto h = compressHash(hashString(s), 20);
    return StorePath(h, name);
}

StorePath StoreDirConfig::makeStorePath(std::string_view type, const Hash & hash, std::string_view name) const
{
    return makeStorePath(type, hash.to_string(HashFormat::Base16, true), name);
}


/* "source" is the type a recursive SHA-256 hash with no references
   gets. Upstream appended the referenced paths to it (they cannot be
   put in <s2> without ambiguity); nothing to append here. */
StorePath StoreDirConfig::makeContentAddressedPath(std::string_view name, const Hash & narHash) const
{
    return makeStorePath("source", narHash, name);
}

} // namespace nix
