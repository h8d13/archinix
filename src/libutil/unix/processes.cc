#include "nix/util/environment-variables.hh"
#include "nix/util/fmt.hh"
#include "nix/util/signals.hh"
#include "nix/util/processes.hh"
#include "nix/util/finally.hh"
#include "nix/util/serialise.hh"

#include <cerrno>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <atomic>
using namespace std::chrono_literals;

#include <grp.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>


#  include <sys/prctl.h>
#  include <sys/mman.h>

#include "util-unix-config-private.hh"

namespace nix {

Pid::Pid() {}

Pid::Pid(Pid && other) noexcept
    : pid(other.pid)
    , separatePG(other.separatePG)
    , killSignal(other.killSignal)
{
    other.release();
}

Pid::Pid(pid_t pid)
    : pid(pid)
{
}

Pid::~Pid()
{
    try {
        if (pid != -1)
            kill(/*allowInterrupts=*/false);
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

void Pid::operator=(pid_t pid)
{
    if (this->pid != -1 && this->pid != pid)
        kill();
    this->pid = pid;
    killSignal = SIGKILL; // reset signal to default
}

Pid::operator pid_t()
{
    return pid;
}

int Pid::kill(bool allowInterrupts)
{
    assert(pid != -1);

    debug("killing process %1%", pid);

    std::atomic<bool> killed = false;

    if (killTimeout > 0ms && killSignal != SIGKILL)
        killThread = std::thread([&]() {
            auto elapsed = 0ms;
            while (elapsed < killTimeout) {
                std::this_thread::sleep_for(25ms);
                elapsed += 25ms;
                if (killed)
                    return;
            }
            ::kill(separatePG ? -pid : pid, SIGKILL);
        });

    /* Send the requested signal to the child.  If it has its own
       process group, send the signal to every process in the child
       process group (which hopefully includes *all* its children). */
    if (::kill(separatePG ? -pid : pid, killSignal) != 0) {
        /* On BSDs, killing a process group will return EPERM if all
           processes in the group are zombies (or something like
           that). So try to detect and ignore that situation. */
            logError(SysError("killing process %d", pid).info());
    }

    int ret = wait(allowInterrupts);
    if (killThread.joinable()) {
        killed = true;
        killThread.join();
    }
    return ret;
}

int Pid::wait(bool allowInterrupts)
{
    assert(pid != -1);
    while (1) {
        int status;
        int res = waitpid(pid, &status, 0);
        if (res == pid) {
            pid = -1;
            return status;
        }
        if (errno != EINTR)
            throw SysError("cannot get exit status of PID %d", pid);
        if (allowInterrupts)
            checkInterrupt();
    }
}

pid_t Pid::release()
{
    pid_t p = pid;
    /* We use the move assignment operator rather than setting the individual fields so we aren't duplicating the
       default values from the header, which would be hard to keep in sync. If we just used the assignment operator
       without manually resetting pid first it would kill that process, however, so we do manually reset that one field.
     */
    pid = -1;
    *this = Pid();
    return p;
}

using ChildWrapperFunction = fun<void()>;

/* Wrapper around vfork to prevent the child process from clobbering
   the caller's stack frame in the parent. */
static pid_t doFork(bool allowVfork, ChildWrapperFunction & fun) __attribute__((noinline));

static pid_t doFork(bool allowVfork, ChildWrapperFunction & fun)
{
    pid_t pid = allowVfork ? vfork() : fork();
    if (pid != 0)
        return pid;
    fun();
    unreachable();
}

} // namespace nix
