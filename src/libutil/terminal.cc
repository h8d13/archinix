#include "nix/util/terminal.hh"

#include <unistd.h>

namespace nix {

bool isTTY()
{
    static const bool tty = isatty(STDERR_FILENO);

    return tty;
}

} // namespace nix
