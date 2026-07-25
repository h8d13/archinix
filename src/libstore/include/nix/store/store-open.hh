#pragma once
/**
 * @file
 *
 * Exactly one store type exists (the local store), so the old
 * URI-parsing registry collapsed into this single constructor-shaped
 * entry point.
 */

#include "nix/store/store-api.hh"

namespace nix {

/**
 * Open the local store rooted at `root`: the store lives at
 * <root>/nix/store with state under <root>/nix/var. `root` must be
 * absolute.
 *
 * A path and nothing else. There is deliberately no settings channel:
 * arch/ is the operator surface, and store knobs are plain defaults in
 * `LocalSettings` that you change by editing and recompiling.
 */
ref<Store> openStore(const std::filesystem::path & root);

} // namespace nix
