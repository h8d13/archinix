#pragma once
/**
 * @file
 *
 * Utilities for working with the current process's environment
 * variables.
 */

#include <optional>

#include "nix/util/types.hh"
#include "nix/util/file-path.hh"

namespace nix {

/**
 * @return an environment variable.
 */
std::optional<std::string> getEnv(const std::string & key);

/**
 * Like `getEnv`, but nullopt when the variable is set to the empty
 * string as well as when it is unset.
 */
std::optional<std::string> getEnvNonEmpty(const std::string & key);

} // namespace nix
