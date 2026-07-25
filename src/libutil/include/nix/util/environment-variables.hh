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
 * Like `getEnv`, but using `OsString` to avoid coercions.
 */
std::optional<OsString> getEnvOs(const OsString & key);

/**
 * Like `getEnv`, but using `OsString` to avoid coercions.
 */
OsStringMap getEnvOs();

/**
 * Like `getEnvNonEmpty`, but using `OsString` to avoid coercions.
 * Returns nullopt if the env variable is not set or set to "".
 */
std::optional<OsString> getEnvOsNonEmpty(const OsString & key);

/**
 * Get the entire environment.
 */
StringMap getEnv();


} // namespace nix
