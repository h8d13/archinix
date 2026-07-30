#pragma once
///@file

#include "nix/util/hash.hh"
#include "nix/store/path.hh"

namespace nix {

/**
 * The content-addressability assertion stored alongside a path
 * (`ValidPathInfo::ca`): the hash the store path was computed from.
 *
 * Upstream carried a method here as well, because a path could be
 * addressed by a flat-file hash, a NAR hash or the `text:` scheme, with
 * or without references. Imports are the only way a path is minted in
 * this store and they all take the same route (NAR, SHA-256, no
 * references), so the method is not a choice and the hash is the whole
 * address. The rendered form keeps the historical `fixed:r:sha256:`
 * spelling: it is what is already written in the `ca` column.
 */
struct ContentAddress
{
    Hash hash;

    bool operator==(const ContentAddress &) const = default;

    std::string render() const;

    static ContentAddress parse(std::string_view rawCa);

    static std::optional<ContentAddress> parseOpt(std::string_view rawCaOpt);
};

/**
 * Render the `ContentAddress` if it exists to a string, return empty
 * string otherwise.
 */
std::string renderContentAddress(std::optional<ContentAddress> ca);

} // namespace nix
