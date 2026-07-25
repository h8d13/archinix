#pragma once
///@file

#include <string>

namespace nix {

/**
 * Determine whether ANSI escape sequences are appropriate for the
 * present output.
 */
bool isTTY();

/**
 * Strip ANSI escape sequences from a string. If 'filterAll' is true,
 * all of them are removed. Otherwise colour-setting sequences are
 * kept. Tabs are expanded to spaces.
 */
std::string filterANSIEscapes(std::string_view s, bool filterAll = false);

} // namespace nix
