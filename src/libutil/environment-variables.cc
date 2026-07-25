#include "nix/util/environment-variables.hh"

namespace nix {

std::optional<std::string> getEnv(const std::string & key)
{
    char * value = getenv(key.c_str());
    if (!value)
        return {};
    return std::string(value);
}

std::optional<OsString> getEnvOsNonEmpty(const OsString & key)
{
    auto value = getEnvOs(key);
    if (value == OS_STR(""))
        return {};
    return value;
}

} // namespace nix
