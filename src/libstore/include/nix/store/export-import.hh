#pragma once

#include "nix/store/store-api.hh"

namespace nix {

/**
 * Magic header of exportPath() output (obsolete).
 */
const uint32_t exportMagic = 0x4558494e;

/**
 * Export multiple paths in the format expected by `nix-store
 * --import`. The paths will be sorted topologically.
 */
void exportPaths(Store & store, const StorePathSet & paths, Sink & sink);

} // namespace nix
