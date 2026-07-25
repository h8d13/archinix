#pragma once
///@file

#include <string>

#include "nix/store/config.hh"

namespace nix {

/**
 * The version of Nix itself.
 */
extern std::string nixVersion;

/**
 * Initialise the library. Must be called before any other libstore
 * function.
 */
void initLibStore();

/**
 * It's important to initialize before doing _anything_, which is why we
 * call upon the programmer to handle this correctly. However, we only add
 * this in a key locations, so as not to litter the code.
 */
void assertLibStoreInitialized();

} // namespace nix
