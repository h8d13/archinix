#pragma once
///@file

#include "nix/util/file-descriptor.hh"


namespace nix {

/**
 * Often we want to use `Descriptor`, but Windows makes a slightly
 * stronger file descriptor vs socket distinction, at least at the level
 * of C types.
 */
using Socket =
    int
    ;


/**
 * Convert a `Descriptor` to a `Socket`
 *
 * This is a no-op except on Windows.
 */
static inline Socket toSocket(Descriptor fd)
{
    return fd;
}

/**
 * Convert a `Socket` to a `Descriptor`
 *
 * This is a no-op except on Windows.
 */
static inline Descriptor fromSocket(Socket fd)
{
    return fd;
}

} // namespace nix
