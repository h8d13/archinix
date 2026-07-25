#pragma once
///@file

#include "nix/util/types.hh"
#include "nix/util/error.hh"
#include "nix/util/fun.hh"
#include "nix/util/logging.hh"

#include <functional>

#  define NIX_SIG_MULTI_INT SIGUSR1

namespace nix {

/* User interruption. */

static inline void setInterrupted(bool isInterrupted);

static inline bool getInterrupted();

static inline bool isInterrupted();

inline void checkInterrupt();

MakeError(Interrupted, BaseError);
MakeError(Cancelled, BaseError);

struct InterruptCallback
{
    virtual ~InterruptCallback();
};

/**
 * Register a function that gets called on SIGINT (in a non-signal
 * context).
 *
 */
std::unique_ptr<InterruptCallback> createInterruptCallback(fun<void()> callback);

/**
 * A RAII class that causes the current thread to receive NIX_SIG_MULTI_INT when
 * the signal handler thread receives SIGINT. That is, this allows
 * SIGINT to be multiplexed to multiple threads.
 *
 */
struct ReceiveInterrupts;

} // namespace nix

#include "nix/util/signals-impl.hh"
