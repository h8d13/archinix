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
    auto h = compressHash(hashString(HashAlgorithm::SHA256, s), 20);
    return StorePath(h, name);
}

StorePath StoreDirConfig::makeStorePath(std::string_view type, const Hash & hash, std::string_view name) const
{
    return makeStorePath(type, hash.to_string(HashFormat::Base16, true), name);
}


/* Stuff the references (if any) into the type.  This is a bit
   hacky, but we can't put them in, say, <s2> (per the grammar above)
   since that would be ambiguous. */
static std::string makeType(const StoreDirConfig & store, std::string && type, const StoreReferences & references)
{
    for (auto & i : references.others) {
        type += ":";
        type += store.printStorePath(i);
    }
    if (references.self)
        type += ":self";
    return std::move(type);
}

StorePath StoreDirConfig::makeFixedOutputPath(std::string_view name, const FixedOutputInfo & info) const
{
    if (info.hash.algo == HashAlgorithm::SHA256 && info.method == FileIngestionMethod::NixArchive) {
        return makeStorePath(makeType(*this, "source", info.references), info.hash, name);
    } else {
        if (!info.references.empty()) {
            throw Error(
                "fixed output derivation '%s' is not allowed to refer to other store paths.\nYou may need to use the 'unsafeDiscardReferences' derivation attribute, see the manual for more details.",
                name);
        }
        // make a unique digest based on the parameters for creating this store object
        auto payload =
            "fixed:out:" + makeFileIngestionPrefix(info.method) + info.hash.to_string(HashFormat::Base16, true) + ":";
        auto digest = hashString(HashAlgorithm::SHA256, payload);
        return makeStorePath("output:out", digest, name);
    }
}

StorePath
StoreDirConfig::makeFixedOutputPathFromCA(std::string_view name, const ContentAddressWithReferences & ca) const
{
    // New template
    return std::visit(
        overloaded{
            [&](const TextInfo & ti) {
                assert(ti.hash.algo == HashAlgorithm::SHA256);
                return makeStorePath(
                    makeType(
                        *this,
                        "text",
                        StoreReferences{
                            .others = ti.references,
                            .self = false,
                        }),
                    ti.hash,
                    name);
            },
            [&](const FixedOutputInfo & foi) { return makeFixedOutputPath(name, foi); }},
        ca.raw);
}

} // namespace nix
