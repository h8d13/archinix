#pragma once
///@file

#include "nix/store/path.hh"
#include "nix/util/hash.hh"
#include "nix/store/content-address.hh"

#include <string>
#include <optional>

namespace nix {

class Store;
struct StoreDirConfig;

/**
 * Information about a store object.
 *
 * See `store/store-object` and `protocols/json/store-object-info` in
 * the Nix manual
 */
struct UnkeyedValidPathInfo
{
    /**
     * The store directory this store object belongs to.
     *
     * This supports relocatable store objects where different objects
     * may have different store directories.
     */
    std::string storeDir;

    /**
     * \todo document this
     */
    Hash narHash;

    /**
     * Other store objects this store object refers to.
     */
    StorePathSet references;

    /**
     * When this store object was registered in the store that contains
     * it, if known.
     */
    time_t registrationTime = 0;

    /**
     * 0 = unknown
     */
    uint64_t narSize = 0;

    /**
     * If set, an assertion that the store path was computed from this
     * hash of the path's contents, so the store does not have to trust
     * anybody's claim about where the path came from: it re-derives
     * the name and compares.
     */
    std::optional<ContentAddress> ca;

    UnkeyedValidPathInfo(const UnkeyedValidPathInfo & other) = default;

    UnkeyedValidPathInfo(const StoreDirConfig & store, Hash narHash);

    UnkeyedValidPathInfo(std::string storeDir, Hash narHash)
        : storeDir(std::move(storeDir))
        , narHash(std::move(narHash))
    {
    }

    /**
     * @param store If non-null, store paths are rendered as full paths.
     *              If null, store paths are rendered as base names.
     * @param includeImpureInfo If true, variable elements such as the
     *                          registration time are included.
     * @param format JSON format version. Version 1 uses string hashes and
     *               string content addresses. Version 2 uses structured
     *               hashes and structured content addresses.
     */
};

struct ValidPathInfo : UnkeyedValidPathInfo
{
    StorePath path;

    /**
     * @return true iff the path is verifiably content-addressed.
     */
    bool isContentAddressed(const StoreDirConfig & store) const;

    ValidPathInfo(StorePath && path, UnkeyedValidPathInfo info)
        : UnkeyedValidPathInfo(info)
        , path(std::move(path))
    {
    }

    ValidPathInfo(const StorePath & path, UnkeyedValidPathInfo info)
        : ValidPathInfo(StorePath{path}, std::move(info))
    {
    }

    /**
     * The info for a path just imported: its store path follows from
     * the NAR hash, which is also its content address.
     */
    static ValidPathInfo makeFromCA(const StoreDirConfig & store, std::string_view name, Hash narHash);
};

static_assert(std::is_move_assignable_v<ValidPathInfo>);
static_assert(std::is_copy_assignable_v<ValidPathInfo>);
static_assert(std::is_copy_constructible_v<ValidPathInfo>);
static_assert(std::is_move_constructible_v<ValidPathInfo>);


} // namespace nix
