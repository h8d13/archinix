#include <algorithm>
#include <cstring>

#include "nix/util/current-process.hh"
#include "nix/util/util.hh"
#include "nix/util/file-system.hh"
#include "nix/util/signals.hh"
#include "nix/util/environment-variables.hh"
#include <math.h>


#  include "nix/util/cgroup.hh"
#  include "nix/util/linux-namespaces.hh"


namespace nix {

unsigned int getMaxCPU()
{
    try {
        auto cgroupFS = linux::getCgroupFS();
        if (!cgroupFS)
            return 0;

        auto cpuFile = *cgroupFS / linux::getCurrentCgroup().rel() / "cpu.max";

        auto cpuMax = readFile(cpuFile);
        auto cpuMaxParts = tokenizeString<std::vector<std::string>>(cpuMax, " \n");

        if (cpuMaxParts.size() != 2) {
            return 0;
        }

        auto quota = cpuMaxParts[0];
        auto period = cpuMaxParts[1];
        if (quota != "max")
            return std::ceil(std::stoi(quota) / std::stof(period));
    } catch (Error &) {
        ignoreExceptionInDestructor(lvlDebug);
    }

    return 0;
}

//////////////////////////////////////////////////////////////////////

size_t savedStackSize = 0;

void ensureStackSizeAtLeast(size_t stackSize)
{
    struct rlimit limit;
    if (getrlimit(RLIMIT_STACK, &limit) == 0 && static_cast<size_t>(limit.rlim_cur) < stackSize) {
        savedStackSize = limit.rlim_cur;
        if (limit.rlim_max < static_cast<rlim_t>(stackSize)) {
            if (getEnv("_NIX_TEST_NO_ENVIRONMENT_WARNINGS") != "1") {
                logger->log(
                    lvlWarn,
                    HintFmt(
                        "Stack size hard limit is %1%, which is less than the desired %2%. If possible, increase the hard limit, e.g. with 'ulimit -Hs %3%'.",
                        limit.rlim_max,
                        stackSize,
                        stackSize / 1024)
                        .str());
            }
        }
        auto requestedSize = std::min(static_cast<rlim_t>(stackSize), limit.rlim_max);
        limit.rlim_cur = requestedSize;
        if (setrlimit(RLIMIT_STACK, &limit) != 0) {
            logger->log(
                lvlError,
                HintFmt(
                    "Failed to increase stack size from %1% to %2% (desired: %3%, maximum allowed: %4%): %5%",
                    savedStackSize,
                    requestedSize,
                    stackSize,
                    limit.rlim_max,
                    std::strerror(errno))
                    .str());
        }
    }
}

void restoreProcessContext(bool restoreMounts)
{
    unix::restoreSignals();
    if (restoreMounts) {
        restoreMountNamespace();
    }

    if (savedStackSize) {
        struct rlimit limit;
        if (getrlimit(RLIMIT_STACK, &limit) == 0) {
            limit.rlim_cur = savedStackSize;
            setrlimit(RLIMIT_STACK, &limit);
        }
    }
}

//////////////////////////////////////////////////////////////////////

std::optional<std::filesystem::path> getSelfExe()
{
    static auto cached = []() -> std::optional<std::filesystem::path> {
        return readLink(std::filesystem::path{"/proc/self/exe"});
    }();
    return cached;
}

} // namespace nix
