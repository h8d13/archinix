#pragma once
///@file

#include "nix/util/types.hh"
#include "nix/util/error.hh"
#include "nix/util/fun.hh"
#include "nix/util/file-descriptor.hh"
#include "nix/util/file-path.hh"
#include "nix/util/logging.hh"
#include "nix/util/ansicolor.hh"
#include "nix/util/os-string.hh"

#include <filesystem>

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <signal.h>

#include <atomic>
#include <functional>
#include <map>
#include <sstream>
#include <optional>
#include <thread>

namespace nix {

struct Sink;
struct Source;

class Pid
{
    pid_t pid = -1;
    bool separatePG = false;
    int killSignal = SIGKILL;
    std::chrono::milliseconds killTimeout;
    std::thread killThread;
public:
    Pid();
    Pid(const Pid &) = delete;
    Pid(Pid && other) noexcept;
    Pid & operator=(const Pid &) = delete;
    Pid & operator=(Pid && other) noexcept;
    Pid(pid_t pid);
    void operator=(pid_t pid);
    operator pid_t();
    ~Pid();
    int kill(bool allowInterrupts = true);
    int wait(bool allowInterrupts = true);

    pid_t release();

    friend void swap(Pid & lhs, Pid & rhs) noexcept
    {
        using std::swap;
        swap(lhs.pid, rhs.pid);
        swap(lhs.separatePG, rhs.separatePG);
        swap(lhs.killSignal, rhs.killSignal);
    }
};


/**
 * Fork a process that runs the given function, and return the child
 * pid to the caller.
 */
struct ProcessOptions
{
    std::string errorPrefix = "";
    bool dieWithParent = true;
    bool runExitHandlers = false;
    bool allowVfork = false;
    /**
     * use clone() with the specified flags (Linux only)
     */
    int cloneFlags = 0;
};


} // namespace nix
