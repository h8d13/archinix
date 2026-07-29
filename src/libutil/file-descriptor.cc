#include "nix/util/file-system.hh"
#include "nix/util/serialise.hh"
#include "nix/util/util.hh"
#include "nix/util/signals.hh"

#include <span>
#include <fcntl.h>
#include <unistd.h>
#include "nix/util/file-system-at.hh"
#include "util-config-private.hh"
#  include <poll.h>

namespace nix {

void EndOfFile::anchor() {}

namespace {

enum class PollDirection { In, Out };

/**
 * Retry an I/O operation if it fails with EAGAIN/EWOULDBLOCK.
 *
 * Polls the fd and retries.
 *
 * This retry logic is needed to handle non-blocking reads/writes. This
 * is needed in the buildhook, because somehow the json logger file
 * descriptor ends up being non-blocking and breaks remote-building.
 *
 * @todo Get rid of buildhook and remove this logic again
 * (https://github.com/NixOS/nix/issues/12688)
 */
template<typename F>
auto retryOnBlock([[maybe_unused]] Descriptor fd, [[maybe_unused]] PollDirection dir, F && f) -> decltype(f())
{
    while (true) {
        try {
            return std::forward<F>(f)();
        } catch (SystemError & e) {
            if (e.is(std::errc::resource_unavailable_try_again) || e.is(std::errc::operation_would_block)) {
                struct pollfd pfd;
                pfd.fd = fd;
                pfd.events = dir == PollDirection::In ? POLLIN : POLLOUT;
                if (poll(&pfd, 1, -1) == -1)
                    throw SysError("poll on file descriptor failed");
                continue;
            }
            throw;
        }
    }
}

} // namespace

void writeFull(Descriptor fd, std::string_view s, bool allowInterrupts)
{
    while (!s.empty()) {
        if (allowInterrupts)
            checkInterrupt();
        auto res = retryOnBlock(fd, PollDirection::Out, [&]() {
            return write(fd, {reinterpret_cast<const std::byte *>(s.data()), s.size()}, allowInterrupts);
        });
        if (res > 0)
            s.remove_prefix(res);
    }
}

std::string readFile(Descriptor fd)
{
    auto size = getFileSize(fd);
    // We can't rely on size being correct, most files in /proc have a nominal size of 0
    return drainFD(fd, {.size = size, .expected = false});
}

void drainFD(Descriptor fd, Sink & sink, DrainFdSinkOpts opts)
{
    // silence GCC maybe-uninitialized warning in finally
    int saved = 0;

    if (!opts.block) {
        saved = fcntl(fd, F_GETFL);
        if (fcntl(fd, F_SETFL, saved | O_NONBLOCK) == -1)
            throw SysError("making file descriptor non-blocking");
    }

    Finally finally([&]() {
        if (!opts.block) {
            if (fcntl(fd, F_SETFL, saved) == -1)
                throw SysError("making file descriptor blocking");
        }
    });

    size_t bytesRead = 0;
    std::array<std::byte, 64 * 1024> buf;
    while (1) {
        checkInterrupt();

        size_t toRead = buf.size();
        if (opts.expectedSize) {
            size_t remaining = *opts.expectedSize - bytesRead;
            if (remaining == 0)
                break;
            toRead = std::min(toRead, remaining);
        }

        size_t n;
        try {
            n = read(fd, std::span(buf.data(), toRead));
        } catch (SystemError & e) {
            if (!opts.block
                && (e.is(std::errc::resource_unavailable_try_again) || e.is(std::errc::operation_would_block)))
                break;
            throw;
        }

        if (n == 0) {
            if (opts.expectedSize && bytesRead < *opts.expectedSize)
                throw EndOfFile("unexpected end-of-file");
            break;
        }

        bytesRead += n;
        sink(std::string_view(reinterpret_cast<const char *>(buf.data()), n));
    }
}

std::string drainFD(Descriptor fd, DrainFdOpts opts)
{
    // the parser needs two extra bytes to append terminating characters, other users will
    // not care very much about the extra memory.
    size_t reserveSize = opts.expected ? 0 : opts.size;
    StringSink sink(reserveSize + 2);
    DrainFdSinkOpts sinkOpts{
        .expectedSize = opts.expected ? std::optional<size_t>(opts.size) : std::nullopt,
        .block = opts.block,
    };
    drainFD(fd, sink, sinkOpts);
    return std::move(sink.s);
}

void copyFdRange(Descriptor fd, off_t offset, size_t nbytes, Sink & sink)
{
    auto left = nbytes;
    std::array<std::byte, 64 * 1024> buf;

    while (left) {
        auto limit = std::min<size_t>(left, buf.size());
        auto n = readOffset(fd, offset, std::span(buf.data(), limit));
        if (n == 0)
            throw EndOfFile("unexpected end-of-file reading from %1%", PathFmt(descriptorToPath(fd)));
        assert(n <= left);
        sink(std::string_view(reinterpret_cast<const char *>(buf.data()), n));
        offset += n;
        left -= n;
    }
}

//////////////////////////////////////////////////////////////////////

AutoCloseFD::AutoCloseFD()
    : fd{INVALID_DESCRIPTOR}
{
}

AutoCloseFD::AutoCloseFD(Descriptor fd)
    : fd{fd}
{
}

// NOTE: This can be noexcept since we are just copying a value and resetting
// the file descriptor in the rhs.
AutoCloseFD::AutoCloseFD(AutoCloseFD && that) noexcept
    : fd{that.fd}
{
    that.fd = INVALID_DESCRIPTOR;
}

// NOLINTNEXTLINE(performance-noexcept-move-constructor) - technically can throw
AutoCloseFD & AutoCloseFD::operator=(AutoCloseFD && that)
{
    close();
    fd = that.fd;
    that.fd = INVALID_DESCRIPTOR;
    return *this;
}

AutoCloseFD::~AutoCloseFD()
{
    try {
        close();
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

Descriptor AutoCloseFD::get() const
{
    return fd;
}

void AutoCloseFD::close()
{
    if (fd != INVALID_DESCRIPTOR) {
        if (
            ::close(fd)
            == -1)
            /* This should never happen. */
            throw NativeSysError("closing file descriptor %1%", fd);
        fd = INVALID_DESCRIPTOR;
    }
}

void AutoCloseFD::startFsync() const
{
    if (fd != -1) {
        /* Ignore failure, since fsync must be run later anyway. This is just a performance optimization. */
        ::sync_file_range(fd, 0, 0, SYNC_FILE_RANGE_WRITE);
    }
}

AutoCloseFD::operator bool() const
{
    return fd != INVALID_DESCRIPTOR;
}

Descriptor AutoCloseFD::release()
{
    Descriptor oldFD = fd;
    fd = INVALID_DESCRIPTOR;
    return oldFD;
}

//////////////////////////////////////////////////////////////////////

void Pipe::close()
{
    readSide.close();
    writeSide.close();
}

std::make_unsigned_t<off_t> getFileSize(Descriptor fd)
{
    auto st = nix::fstat(fd);
    return st.st_size;
}

size_t read(Descriptor fd, std::span<std::byte> buffer)
{
    ssize_t n;
    do {
        checkInterrupt();
        n = ::read(fd, buffer.data(), buffer.size());
    } while (n == -1 && errno == EINTR);
    if (n == -1)
        throw SysError("read of %1% bytes", buffer.size());
    return static_cast<size_t>(n);
}

size_t readOffset(Descriptor fd, off_t offset, std::span<std::byte> buffer)
{
    ssize_t n;
    do {
        checkInterrupt();
        n = ::pread(fd, buffer.data(), buffer.size(), offset);
    } while (n == -1 && errno == EINTR);
    if (n == -1)
        throw SysError("pread of %1% bytes at offset %2%", buffer.size(), offset);
    return static_cast<size_t>(n);
}

size_t write(Descriptor fd, std::span<const std::byte> buffer, bool allowInterrupts)
{
    ssize_t n;
    do {
        if (allowInterrupts)
            checkInterrupt();
        n = ::write(fd, buffer.data(), buffer.size());
    } while (n == -1 && errno == EINTR);
    if (n == -1)
        throw SysError("write of %1% bytes", buffer.size());
    return static_cast<size_t>(n);
}

//////////////////////////////////////////////////////////////////////

void Pipe::create(bool nonBlocking)
{
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | (nonBlocking ? O_NONBLOCK : 0)) != 0)
        throw SysError("creating pipe");
    readSide = fds[0];
    writeSide = fds[1];
}

//////////////////////////////////////////////////////////////////////

void closeOnExec(int fd)
{
    int prev;
    if ((prev = fcntl(fd, F_GETFD, 0)) == -1 || fcntl(fd, F_SETFD, prev | FD_CLOEXEC) == -1)
        throw SysError("setting close-on-exec flag");
}

void syncDescriptor(Descriptor fd)
{
    int result =
        ::fsync(fd)
        ;
    if (result == -1)
        throw NativeSysError("fsync file descriptor %1%", fd);
}

} // namespace nix
