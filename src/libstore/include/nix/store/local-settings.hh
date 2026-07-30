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
 *
 * Held by the store's config, so these are genuinely per-store rather
 * than the global pretence they used to be.
 */
struct LocalSettings
{
    /**
     * Amount of reserved disk space for the garbage collector.
     * Upstream split this into a `GCSettings` base reached through
     * `getGCSettings()`, for a collector that was its own store
     * capability; there is one settings struct and one reader.
     */
    off_t reservedSize = 8 * 1024 * 1024;

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
     * Threads issuing the import's dedup swaps (`link` + `rename` over
     * a file whose content the link farm already holds). 0 does them
     * inline on the hashing thread, which is where they used to live.
     *
     * This is not a hashing knob: hashing stays one thread, since the
     * digest that names a store path is a serial pass over one stream.
     * The swaps are device round trips, which is what overlaps.
     *
     * One is the measured default (see bench/BASELINE): the gain is
     * getting the swaps off the hashing thread, not issuing them
     * concurrently, and more threads were flat to worse on NVMe. A
     * device with millisecond metadata ops may want two or three, which
     * is what the knob is for.
     */
    size_t dedupThreads = 1;

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
