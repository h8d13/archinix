#include "nix/store/globals.hh"
#include "nix/util/logging.hh"
#include "nix/util/signals.hh"
#include "nix/util/util.hh"


namespace nix {

static bool initLibStoreDone = false;

void assertLibStoreInitialized()
{
    if (!initLibStoreDone) {
        printError("The program must call nix::initNix() before calling any libstore library functions.");
        abort();
    };
}

void initLibStore()
{
    if (initLibStoreDone)
        return;

    initLibUtil();

    /* Without this nothing ever sets the interrupt flag, so every
       checkInterrupt() on the import/gc/optimise paths is a no-op and
       Ctrl-C kills the process mid-write instead of unwinding. */
    startSignalHandlerThread();

    initLibStoreDone = true;
}

} // namespace nix
