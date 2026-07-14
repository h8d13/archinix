#pragma once
///@file

#include <thread>
#include <cassert>

#include <poll.h>
#include <errno.h>


#include "nix/util/signals.hh"
#include "nix/util/file-descriptor.hh"

namespace nix {

class MonitorFdHup
{
private:
    std::thread thread;
    Pipe notifyPipe;

    void runThread(int watchFd, int notifyFd);

public:
    MonitorFdHup(int fd);
    MonitorFdHup(MonitorFdHup &&) = delete;
    MonitorFdHup(const MonitorFdHup &) = delete;
    MonitorFdHup & operator=(MonitorFdHup &&) = delete;
    MonitorFdHup & operator=(const MonitorFdHup &) = delete;

    ~MonitorFdHup()
    {
        // Close the write side to signal termination via POLLHUP
        notifyPipe.writeSide.close();
        thread.join();
    }
};

inline void MonitorFdHup::runThread(int watchFd, int notifyFd)
{
    while (true) {
        struct pollfd fds[2];
        fds[0].fd = watchFd;
        fds[0].events = 0; // POSIX: POLLHUP is always reported
        fds[1].fd = notifyFd;
        fds[1].events = 0;

        auto count = poll(fds, 2, -1);
        if (count == -1) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            } else {
                throw SysError("in MonitorFdHup poll()");
            }
        }

        if (fds[0].revents & POLLHUP) {
            unix::triggerInterrupt();
            break;
        }

        if (fds[1].revents & POLLHUP) {
            // Notify pipe closed, exit thread
            break;
        }
    }
}

inline MonitorFdHup::MonitorFdHup(int fd)
{
    notifyPipe.create();
    int notifyFd = notifyPipe.readSide.get();
    thread = std::thread([this, fd, notifyFd]() { this->runThread(fd, notifyFd); });
};

} // namespace nix
