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

/**
 * The reference set that follows each path in the stream: a count,
 * then one printed store path per entry.
 *
 * Upstream reached these through `CommonProto::Serialise<T>`, a
 * serialiser table parameterised over the protocol (worker, serve) and
 * specialised for vector, set, tuple and map. One protocol and one
 * container survived the extraction, so the table, the
 * `LengthPrefixedProtoHelper` it dispatched to and the `Inner`
 * template parameter that picked between protocols are gone: this pair
 * is the whole of what they ever serialised here.
 */
StorePathSet readStorePathSet(const StoreDirConfig & store, Source & from);
void writeStorePathSet(const StoreDirConfig & store, Sink & to, const StorePathSet & paths);

} // namespace nix
