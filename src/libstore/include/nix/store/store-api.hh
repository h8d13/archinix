#pragma once
/**
 * @file
 *
 * Types shared across the store layer. The store itself is one
 * concrete class, `LocalStore` (see local-store.hh): there is no store
 * interface to implement, so what used to be this header's `Store` and
 * `StoreConfig` live there.
 */

#include "nix/store/path.hh"
#include "nix/util/hash.hh"
#include "nix/store/content-address.hh"
#include "nix/util/serialise.hh"
#include "nix/util/sync.hh"
#include "nix/store/path-info.hh"
#include "nix/store/store-dir-config.hh"
#include "nix/util/source-path.hh"

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <chrono>

namespace nix {

MakeError(InvalidPath, Error);

struct SourceAccessor;

} // namespace nix
