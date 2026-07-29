#pragma once
/**
 * @file
 *
 * @brief File descriptor operations for almost arbitrary file
 * descriptors.
 *
 * More specialized file-system-specific operations are in
 * @ref file-system-at.hh.
 */

#include "nix/util/canon-path.hh"
#include "nix/util/error.hh"
#include "nix/util/os-string.hh"


namespace nix {

struct Sink;
struct Source;

/**
 * Operating System capability
 */
using Descriptor =
    int
    ;

const Descriptor INVALID_DESCRIPTOR =
    -1
    ;

/**
 * Read the contents of a resource into a string.
 */
std::string readFile(Descriptor fd);

/**
 * Platform-specific read into a buffer.
 *
 * Thin wrapper around ::read. Handles EINTR.
 *
 * @param fd The file descriptor to read from
 * @param buffer The buffer to read into
 * @return The number of bytes actually read (0 indicates EOF)
 * @throws SystemError on failure
 */
size_t read(Descriptor fd, std::span<std::byte> buffer);

/**
 * Platform-specific write from a buffer.
 *
 * Thin wrapper around ::write.
 * Handles EINTR on Unix.
 *
 * @param fd The file descriptor to write to
 * @param buffer The buffer to write from
 * @return The number of bytes actually written
 * @throws SystemError on failure
 */
size_t write(Descriptor fd, std::span<const std::byte> buffer, bool allowInterrupts);

/**
 * Get the size of a file.
 *
 * Thin wrapper around fstat.
 *
 * @param fd The file descriptor
 * @return The file size
 * @throws SystemError on failure
 */
std::make_unsigned_t<off_t> getFileSize(Descriptor fd);

/**
 * Platform-specific positioned read into a buffer.
 *
 * Thin wrapper around pread.
 * Does NOT handle EINTR on Unix - caller must catch and retry if needed.
 *
 * @param fd The file descriptor to read from (must be seekable)
 * @param offset The offset to read from
 * @param buffer The buffer to read into
 * @return The number of bytes actually read (0 indicates EOF)
 * @throws SystemError on failure
 */
size_t readOffset(Descriptor fd, off_t offset, std::span<std::byte> buffer);

/**
 * Read \ref nbytes starting at \ref offset from a seekable file into a sink.
 *
 * @throws SystemError if fd is not seekable or any operation fails
 * @throws Interrupted if the operation was interrupted
 * @throws EndOfFile if an EOF was reached before reading \ref nbytes
 */
void copyFdRange(Descriptor fd, off_t offset, size_t nbytes, Sink & sink);

void writeFull(Descriptor fd, std::string_view s, bool allowInterrupts = true);

/**
 * Perform a blocking fsync operation on a file descriptor.
 */
void syncDescriptor(Descriptor fd);

/**
 * Options for draining a file descriptor to a sink.
 */
struct DrainFdSinkOpts
{
    /**
     * If provided, read exactly this many bytes (throws EndOfFile if EOF occurs before reading all bytes).
     */
    std::optional<std::make_unsigned_t<off_t>> expectedSize = {};

    /**
     * Whether to block on read.
     */
    bool block = true;
};

/**
 * Options for draining a file descriptor to a string.
 */
struct DrainFdOpts
{
    /**
     * If expected=true: read exactly this many bytes (throws EndOfFile if EOF occurs before reading all bytes).
     * If expected=false: size hint for string allocation.
     */
    std::make_unsigned_t<off_t> size = 0;

    /**
     * If true, size is exact expected size. If false, size is just a reservation hint.
     */
    bool expected = false;

    /**
     * Whether to block on read.
     */
    bool block = true;
};

/**
 * Read a file descriptor until EOF occurs.
 *
 * @param fd The file descriptor to drain
 * @param opts Options for the drain operation
 */
std::string drainFD(Descriptor fd, DrainFdOpts opts = {});

/**
 * Read a file descriptor until EOF occurs, writing to a sink.
 *
 * @param fd The file descriptor to drain
 * @param sink The sink to write data to
 * @param opts Options for the drain operation
 */
void drainFD(Descriptor fd, Sink & sink, DrainFdSinkOpts opts = {});

/**
 * Get [Standard Input](https://en.wikipedia.org/wiki/Standard_streams#Standard_input_(stdin))
 */
[[gnu::always_inline]]
inline Descriptor getStandardInput()
{
    return STDIN_FILENO;
}

/**
 * Get [Standard Output](https://en.wikipedia.org/wiki/Standard_streams#Standard_output_(stdout))
 */
[[gnu::always_inline]]
inline Descriptor getStandardOutput()
{
    return STDOUT_FILENO;
}

/**
 * Get [Standard Error](https://en.wikipedia.org/wiki/Standard_streams#Standard_error_(stderr))
 */
[[gnu::always_inline]]
inline Descriptor getStandardError()
{
    return STDERR_FILENO;
}

/**
 * Automatic cleanup of resources.
 */
class AutoCloseFD
{
    Descriptor fd;
public:
    AutoCloseFD();
    AutoCloseFD(Descriptor fd);
    AutoCloseFD(const AutoCloseFD & fd) = delete;
    AutoCloseFD(AutoCloseFD && fd) noexcept;
    ~AutoCloseFD();
    AutoCloseFD & operator=(const AutoCloseFD & fd) = delete;
    // NOLINTNEXTLINE(performance-noexcept-move-constructor) - technically can throw because of close()
    AutoCloseFD & operator=(AutoCloseFD && fd);
    Descriptor get() const;
    explicit operator bool() const;
    Descriptor release();
    void close();

    /**
     * Perform a blocking fsync operation.
     */
    void fsync() const
    {
        if (fd != INVALID_DESCRIPTOR)
            nix::syncDescriptor(fd);
    }

    /**
     * Asynchronously flush to disk without blocking, if available on
     * the platform. This is just a performance optimization, and
     * fsync must be run later even if this is called.
     */
    void startFsync() const;
};

class Pipe
{
public:
    AutoCloseFD readSide, writeSide;

    void create(
        bool nonBlocking = false
    );

    void close();
};

/**
 * Set the close-on-exec flag for the given file descriptor.
 */
void closeOnExec(Descriptor fd);

MakeError(EndOfFile, Error);


} // namespace nix
