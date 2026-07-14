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

    // TODO: Implement for Windows
    void setSeparatePG(bool separatePG);
    void setKillSignal(int signal);
    void setKillTimeout(std::chrono::milliseconds duration);
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
 * Kill all processes running under the specified uid by sending them
 * a SIGKILL.
 */
void killUser(uid_t uid);

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

pid_t startProcess(fun<void()> processMain, const ProcessOptions & options = ProcessOptions());

/**
 * Run a program and return its stdout in a string (i.e., like the
 * shell backtick operator).
 */
std::string runProgram(
    std::filesystem::path program,
    bool lookupPath = false,
    const OsStrings & args = OsStrings(),
    bool isInteractive = false);

struct RunOptions
{
    std::filesystem::path program;
    bool lookupPath = true;
    OsStrings args;
    std::optional<uid_t> uid;
    std::optional<uid_t> gid;
    std::optional<std::filesystem::path> chdir;
    std::optional<OsStringMap> environment;
    Sink * standardOut = nullptr;
    bool mergeStderrToStdout = false;
    bool isInteractive = false;
};

// Output = error code + "standard out" output stream
std::pair<int, std::string> runProgram(RunOptions && options);

void runProgram2(const RunOptions & options);

class ExecError final : public CloneableError<ExecError, Error>
{
    void anchor() override;

public:
    int status;

    template<typename... Args>
    ExecError(int status, const Args &... args)
        : CloneableError(args...)
        , status(status)
    {
    }
};

/**
 * Convert the exit status of a child as returned by wait() into an
 * error string.
 */
std::string statusToString(int status);

bool statusOk(int status);

} // namespace nix
