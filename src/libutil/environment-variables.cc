#include "nix/util/environment-variables.hh"

#include <cstdlib>

namespace nix {

std::optional<std::string> getEnv(const std::string & key)
{
    char * value = getenv(key.c_str());
    if (!value)
        return {};
    return std::string(value);
}

std::optional<std::string> getEnvNonEmpty(const std::string & key)
{
    auto value = getEnv(key);
    if (value == "")
        return {};
    return value;
}

} // namespace nix
