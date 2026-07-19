#pragma once
///@file

#include "nix/util/types.hh"

namespace nix {

/**
 * Percent-encode `s` per RFC 3986: unreserved characters
 * (ALPHA / DIGIT / `-` / `.` / `_` / `~`) and anything in `keep` pass
 * through literally, everything else becomes `%XX`.
 *
 * All that survived of URL handling: SQLite `file:` URIs and rendering
 * store config params for logs. Full parsing left with the store
 * registry.
 */
std::string percentEncode(std::string_view s, std::string_view keep = "");

/**
 * Render a query map as `k=v&k2=v2` with percent-encoding.
 */
std::string encodeQuery(const StringMap & query);

} // namespace nix
