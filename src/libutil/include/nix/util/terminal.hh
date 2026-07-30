#pragma once
///@file

namespace nix {

/**
 * Whether stderr is an interactive terminal. Gates the in-place
 * progress counter, which would otherwise scribble into a log file or
 * the journal. Upstream also consulted TERM/NO_COLOR/NOCOLOR here,
 * which were about colour; nothing emits colour.
 */
bool isTTY();

} // namespace nix
