#pragma once
/**
 * @file
 *
 * Exactly one store type exists (the local store), so the old
 * URI-parsing registry collapsed into this single constructor-shaped
 * entry point.
 */

#include "nix/store/local-store.hh"

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
/**
 * Open the local store rooted at `root`.
 *
 * @param mustExist refuse a root that has no store yet, instead of
 * creating one. Opening a store creates its whole skeleton, so a
 * read-only query against a typo, or against a mountpoint whose disk
 * is not attached, would otherwise report an empty store and succeed.
 * Writers leave this false: a blank disk gets its store on first
 * import.
 */
ref<LocalStore> openStore(const std::filesystem::path & root, bool mustExist = false);

} // namespace nix
