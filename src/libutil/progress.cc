#include "nix/util/progress.hh"
#include "nix/util/terminal.hh"

#include <chrono>
#include <cstdio>
#include <mutex>

namespace nix {

namespace {

/* optimisePath_ also runs under a ThreadPool for whole-store passes,
   so the throttle and the open-line flag are shared state, not
   per-caller */
std::mutex progressMutex;
bool lineOpen = false;
std::chrono::steady_clock::time_point lastPaint;

/* fast enough to look live, slow enough that a 35k-file import pays
   for a few dozen writes instead of 35k */
constexpr auto paintEvery = std::chrono::milliseconds(80);

} // namespace

void progressTick(std::string_view what, uint64_t done, uint64_t total)
{
    if (!isTTY())
        return;

    /* a total that turned out too low degrades to the bare count
       rather than printing past 100% */
    if (done > total)
        total = 0;

    std::lock_guard<std::mutex> lock(progressMutex);
    auto now = std::chrono::steady_clock::now();
    /* the first tick of a phase paints immediately (a phase shorter
       than the interval would otherwise never show at all) and so
       does the last one, so a finished phase reads 100% instead of
       whatever the throttle last let through */
    bool edge = !lineOpen || (total && done == total);
    if (!edge && now - lastPaint < paintEvery)
        return;
    lastPaint = now;
    lineOpen = true;

    if (total)
        fprintf(
            stderr,
            "\r%.*s: %ju/%ju (%ju%%)",
            (int) what.size(),
            what.data(),
            (uintmax_t) done,
            (uintmax_t) total,
            (uintmax_t) (done * 100 / total));
    else
        fprintf(stderr, "\r%.*s: %ju", (int) what.size(), what.data(), (uintmax_t) done);
}

void progressEnd()
{
    if (!isTTY())
        return;

    std::lock_guard<std::mutex> lock(progressMutex);
    if (!lineOpen)
        return;
    fputc('\n', stderr);
    lineOpen = false;
}

} // namespace nix
