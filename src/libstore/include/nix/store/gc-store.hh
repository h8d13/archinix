#pragma once
/**
 * @file
 *
 * Garbage collection options and results. The collector itself is
 * `LocalStore::collectGarbage`: with one store type there is no
 * gc-capability mixin to dispatch through.
 */

#include "nix/store/store-api.hh"

namespace nix {

/**
 * Store paths that a symlink under `<state>/gcroots` points at.
 * Which link named them is not kept: the collector only asks whether a
 * path is rooted, and rm-path unroots before it deletes.
 */
using Roots = StorePathSet;

/**
 * What to delete. There is one collection mode: the paths named here
 * go, or the call fails saying why. No whole-store sweep (nothing
 * becomes garbage on its own here: a generation stays rooted until
 * rm-path unroots it) and no live/dead query (rooted is the whole
 * answer, references do not exist).
 */
struct GCOptions
{
    StorePathSet paths;
};

struct GCResults
{
    /**
     * The paths that were deleted.
     */
    StringSet paths;

    /**
     * The number of bytes that were freed.
     */
    uint64_t bytesFreed = 0;
};

} // namespace nix
