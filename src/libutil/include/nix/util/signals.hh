#pragma once
/**
 * @file
 *
 * User interruption: a signal handler thread flips a flag that
 * long-running loops poll through `checkInterrupt()`.
 */

#include "nix/util/types.hh"
#include "nix/util/error.hh"
#include "nix/util/fun.hh"
#include "nix/util/logging.hh"

#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <functional>

/**
 * Signal used to multiplex SIGINT to the threads that asked for it via
 * `ReceiveInterrupts`.
 */
#define NIX_SIG_MULTI_INT SIGUSR1

namespace nix {

MakeError(Interrupted, BaseError);
MakeError(Cancelled, BaseError);

extern std::atomic<bool> _isInterrupted;

extern thread_local std::function<bool()> interruptCheck;

void _interrupted();

/**
 * Start a thread that handles various signals. Also block those signals
 * on the current thread (and thus any threads created by it).
 * Saves the signal mask before changing the mask to block those signals.
 * See saveSignalMask().
 */
void startSignalHandlerThread();

static inline bool isInterrupted()
{
    return _isInterrupted || (interruptCheck && interruptCheck());
}

/**
 * Throw `Interrupted` exception if the process has been interrupted.
 *
 * Call this in long-running loops and between slow operations to terminate
 * them as needed.
 */
inline void checkInterrupt()
{
    if (isInterrupted())
        _interrupted();
}

struct InterruptCallback
{
    virtual ~InterruptCallback();
};

/**
 * Register a function that gets called on SIGINT (in a non-signal
 * context).
 */
std::unique_ptr<InterruptCallback> createInterruptCallback(fun<void()> callback);

/**
 * A RAII class that causes the current thread to receive NIX_SIG_MULTI_INT when
 * the signal handler thread receives SIGINT. That is, this allows
 * SIGINT to be multiplexed to multiple threads.
 */
struct ReceiveInterrupts
{
    pthread_t target;
    std::unique_ptr<InterruptCallback> callback;

    ReceiveInterrupts()
        : target(pthread_self())
        , callback(createInterruptCallback([&]() { pthread_kill(target, NIX_SIG_MULTI_INT); }))
    {
    }
};

} // namespace nix
