#pragma once

#include "nix/store/local-store.hh"

namespace nix {

/**
 * Follows the NAR of one path in an exportPaths() stream: what the
 * reader checks before believing the fields after it. Distinct from
 * upstream's value because the stream is not upstream's.
 */
const uint32_t exportMagic = 0x4558494f;

/**
 * Export multiple paths as one stream, `arch/import-path` reads it
 * back. No order is imposed: the paths are reference-free.
 */
void exportPaths(LocalStore & store, const StorePathSet & paths, Sink & sink);

} // namespace nix
