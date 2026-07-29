#include "nix/util/signals.hh"
#include "nix/util/util.hh"
#include "nix/util/error.hh"
#include "nix/util/fun.hh"
#include "nix/util/sync.hh"

#include <thread>

namespace nix {

void Interrupted::anchor() {}

void Cancelled::anchor() {}

std::atomic<bool> _isInterrupted = false;

thread_local std::function<bool()> interruptCheck;

void _interrupted()
{
    /* Block user interrupts while an exception is being handled.
       Throwing an exception while another exception is being handled
       kills the program! */
    if (!std::uncaught_exceptions()) {
        throw Interrupted("interrupted by the user");
    }
}

//////////////////////////////////////////////////////////////////////

/* We keep track of interrupt callbacks using integer tokens, so we can iterate
   safely without having to lock the data structure while executing arbitrary
   functions.
 */
struct InterruptCallbacks
{
    typedef int64_t Token;

    /* We use unique tokens so that we can't accidentally delete the wrong
       handler because of an erroneous double delete. */
    Token nextToken = 0;

    /* Used as a list, see InterruptCallbacks comment. */
    std::map<Token, fun<void()>> callbacks;
};

InterruptCallback::~InterruptCallback() {}

/* Required to avoid static initialization order fiasco. This allows global
   objects to safely register callbacks. */
static Sync<InterruptCallbacks> & getInterruptCallbacks()
{
    /* Intentionally leak, according to the Construct On First Use Idiom.
       An alternative is to use the Nifty Counter Idiom, but
       InterruptCallbacks' destructor is not very important. */
    static Sync<InterruptCallbacks> * _interruptCallbacks = new Sync<InterruptCallbacks>();
    return *_interruptCallbacks;
}

/* Only the signal handler thread raises interrupts. */
static void triggerInterrupt();

static void signalHandlerThread(sigset_t set)
{
    while (true) {
        int signal = 0;
        sigwait(&set, &signal);

        if (signal == SIGINT || signal == SIGTERM || signal == SIGHUP)
            triggerInterrupt();
    }
}

static void triggerInterrupt()
{
    _isInterrupted = true;

    {
        InterruptCallbacks::Token i = 0;
        while (true) {
            std::function<void()> callback;
            {
                auto interruptCallbacks(getInterruptCallbacks().lock());
                auto lb = interruptCallbacks->callbacks.lower_bound(i);
                if (lb == interruptCallbacks->callbacks.end())
                    break;

                callback = lb->second;
                i = lb->first + 1;
            }

            try {
                callback();
            } catch (...) {
                ignoreExceptionInDestructor();
            }
        }
    }
}

void startSignalHandlerThread()
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &set, nullptr))
        throw SysError("blocking signals");

    std::thread(signalHandlerThread, set).detach();
}

namespace {

/* RAII helper to automatically deregister a callback. */
struct InterruptCallbackImpl : InterruptCallback
{
    InterruptCallbacks::Token token;

    InterruptCallbackImpl(InterruptCallbacks::Token token)
        : token(token)
    {
    }

    InterruptCallbackImpl(InterruptCallbackImpl &&) = delete;
    InterruptCallbackImpl(const InterruptCallbackImpl &) = delete;
    InterruptCallbackImpl & operator=(InterruptCallbackImpl &&) = delete;
    InterruptCallbackImpl & operator=(const InterruptCallbackImpl &) = delete;

    ~InterruptCallbackImpl() override
    {
        auto interruptCallbacks(getInterruptCallbacks().lock());
        interruptCallbacks->callbacks.erase(token);
    }
};

} // namespace

std::unique_ptr<InterruptCallback> createInterruptCallback(fun<void()> callback)
{
    auto interruptCallbacks(getInterruptCallbacks().lock());
    auto token = interruptCallbacks->nextToken++;
    interruptCallbacks->callbacks.emplace(token, callback);
    return std::make_unique<InterruptCallbackImpl>(token);
}

} // namespace nix
