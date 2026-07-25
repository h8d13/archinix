#include "nix/util/users.hh"

#include <unistd.h>

namespace nix {

bool isRootUser()
{
    return getuid() == 0;
}

} // namespace nix
