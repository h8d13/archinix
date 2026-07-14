#pragma once
///@file

#include "nix/util/file-descriptor.hh"
#include "nix/util/fun.hh"

#  include <poll.h>

namespace nix {

/**
 * An "muxable pipe" is a type of pipe supporting endpoints that wait
 * for events on multiple pipes at once.
 *
 * On Unix, this is just a regular anonymous pipe. On Windows, this has
 * to be a named pipe because we need I/O Completion Ports to wait on
 * multiple pipes.
 */
using MuxablePipe =
    Pipe
    ;

/**
 * Use pool() (Unix) / I/O Completion Ports (Windows) to wait for the
 * input side of any logger pipe to become `available'.  Note that
 * `available' (i.e., non-blocking) includes EOF.
 */
struct MuxablePipePollState
{
    std::vector<struct pollfd> pollStatus;
    std::map<int, size_t> fdToPollStatus;

    /**
     * Check for ready (Unix) / completed (Windows) operations
     */
    void poll(
        std::optional<unsigned int> timeout);

    using CommChannel =
        Descriptor
        ;

    /**
     * Process for ready (Unix) / completed (Windows) operations,
     * calling the callbacks as needed.
     *
     * @param handleRead callback to be passed read data.
     *
     * @param handleEOF callback for when the `MuxablePipe` has closed.
     */
    void iterate(
        std::set<CommChannel> & channels,
        fun<void(Descriptor fd, std::string_view data)> handleRead,
        fun<void(Descriptor fd)> handleEOF);
};

} // namespace nix
