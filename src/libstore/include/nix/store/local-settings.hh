#pragma once
///@file

#include "nix/util/types.hh"

#include "nix/store/config.hh"

#include <cstddef>
#include <cstdint>
#include <sys/types.h>

namespace nix {

/**
 * Store knobs. These are plain fields with defaults, not a
 * configuration system: the store is opened with a path and nothing
 * else, and arch/ is the operator surface. Change a default here and
 * recompile; there is deliberately no file, env var or URI parameter
 * that can reach them at runtime.
 */
struct GCSettings
{
    /**
     * Amount of reserved disk space for the garbage collector.
     */
    off_t reservedSize = 8 * 1024 * 1024;
};

/**
 * Settings for a local store. Held by the store's config, so these are
 * genuinely per-store rather than the global pretence they used to be.
 */
struct LocalSettings : GCSettings
{
    GCSettings & getGCSettings()
    {
        return *this;
    }

    const GCSettings & getGCSettings() const
    {
        return *this;
    }

    /**
     * Whether changes to the store metadata (in `nix/var/nix/db`) are
     * synchronously flushed to disk. Robust across crashes, slower.
     */
    bool fsyncMetadata = true;

    /**
     * Whether to `fsync()` store paths before registering them.
     */
    bool fsyncStorePaths = false;

    /**
     * Whether to call `sync()` before registering a path as valid.
     * `fsyncStorePaths` is the faster way to get the same guarantee.
     */
    bool syncBeforeRegistering = false;

    /**
     * Hard-link identical files on import. Note that import-dir and
     * import-path call `optimisePath()` directly, which does not
     * consult this.
     */
    bool autoOptimiseStore = false;

    /**
     * Maximum size of NARs before spilling them to disk.
     */
    size_t narBufferSize = 32 * 1024 * 1024;

    /**
     * Tolerate symlink components in the store directory path.
     */
    bool allowSymlinkedStore = false;

    /**
     * Whether SQLite should use WAL mode.
     */
    bool useSQLiteWAL = true;

#if NIX_SUPPORT_ACL
    /**
     * ACLs to leave alone. Import strips ACLs from store paths, but
     * some (`security.selinux`, `system.nfs4_acl`) cannot be removed
     * even by root.
     */
    StringSet ignoredAcls{"security.selinux", "system.nfs4_acl", "security.csm"};
#endif
};

} // namespace nix
