#include "nix/util/file-system.hh"
#include "nix/util/file-system-at.hh"
#include "nix/util/signals.hh"

#include <fcntl.h>
#include <unistd.h>
#include <span>

#include "util-unix-config-private.hh"

namespace nix {

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
#if HAVE_PIPE2
    if (pipe2(fds, O_CLOEXEC | (nonBlocking ? O_NONBLOCK : 0)) != 0)
        throw SysError("creating pipe");
#else
    if (pipe(fds) != 0)
        throw SysError("creating pipe");
    for (auto fd : fds) {
        unix::closeOnExec(fd);
        if (nonBlocking && ::fcntl(fd, F_SETFL, O_NONBLOCK) == -1)
            throw SysError("making pipe non-blocking");
    }
#endif
    readSide = fds[0];
    writeSide = fds[1];
}

//////////////////////////////////////////////////////////////////////

void unix::closeOnExec(int fd)
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
