#pragma once
///@file

#include <cstdint>
#include <string_view>

namespace nix {

/**
 * A counter line on stderr, rewritten in place while a long loop runs:
 * the import and optimise passes walk tens of thousands of files with
 * nothing on the console until they finish.
 *
 * No-op unless stderr is a terminal (`isTTY`), which keeps it out of
 * the ISO and test logs and off stdout, where the tools put their real
 * output: import-dir prints the store path there and nixgen-commit
 * captures it. Throttled, so the cost is a handful of writes rather
 * than one per file.
 *
 * Counts only ever grow and each phase ends with progressEnd(), so a
 * repaint never has to erase a longer previous line: no escapes, works
 * on the serial console.
 *
 * @param total 0 when the loop's length is not known up front. Prints
 * a bare running count, as git does while it is still counting. A
 * total the count overruns is treated the same way, so an estimated
 * one can never render past 100%.
 */
void progressTick(std::string_view what, uint64_t done, uint64_t total = 0);

/**
 * Close an open counter line so ordinary output starts on its own
 * line. No-op when nothing is open, so callers can end a phase
 * unconditionally.
 */
void progressEnd();

} // namespace nix
