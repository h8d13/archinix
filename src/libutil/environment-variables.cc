#include "nix/util/environment-variables.hh"
#include <cstdlib>
#include <cstring>
extern char ** environ __attribute__((weak));

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

std::optional<std::string> getEnvOs(const std::string & key)
{
    return getEnv(key);
}

OsStringMap getEnvOs()
{
    OsStringMap env;
    for (size_t i = 0; environ[i]; ++i) {
        auto s = environ[i];
        auto eq = strchr(s, '=');
        if (!eq)
            // invalid env, just keep going
            continue;
        env.emplace(std::string(s, eq), std::string(eq + 1));
    }
    return env;
}

StringMap getEnv()
{
    return getEnvOs();
}

} // namespace nix
