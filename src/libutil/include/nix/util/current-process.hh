#pragma once
///@file

#include <filesystem>
#include <optional>
#include <chrono>

#  include <sys/resource.h>

#include "nix/util/types.hh"

namespace nix {

/**
 * Get the current process's user space CPU time.
 */
std::chrono::microseconds getCpuUserTime();


// It does not seem possible to dynamically change stack size on Windows.
/**
 * Increase the RLIMIT_STACK rlimit if it is currently smaller than `stackSize`.
 * @note Not thread safe. Calls to this should be wrapped in a std::call_once.
 */
void ensureStackSizeAtLeast(size_t stackSize);

/**
 * Restore the original inherited Unix process context (such as signal
 * masks, stack size).

 * See unix::startSignalHandlerThread(), unix::saveSignalMask().
 */
void restoreProcessContext(bool restoreMounts = true);

/**
 * @return the path of the current executable.
 */
std::optional<std::filesystem::path> getSelfExe();

} // namespace nix
