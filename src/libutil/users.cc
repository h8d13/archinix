#include "nix/util/users.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/file-system.hh"

#  include "unix/xdg-dirs.hh"

namespace nix {

std::filesystem::path getCacheDir()
{
    auto dir = getEnvOs(OS_STR("NIX_CACHE_HOME"));
    if (dir)
        return *dir;
    return unix::xdg::getCacheHome() / "nix";
}

std::filesystem::path getConfigDir()
{
    auto dir = getEnvOs(OS_STR("NIX_CONFIG_HOME"));
    if (dir)
        return *dir;
    return unix::xdg::getConfigHome() / "nix";
}

std::vector<std::filesystem::path> getConfigDirs()
{
    std::filesystem::path configHome = getConfigDir();
    std::vector<std::filesystem::path> result;
    result.push_back(configHome);
    auto xdgConfigDirs = unix::xdg::getConfigDirs();
    for (auto & dir : xdgConfigDirs) {
        result.push_back(dir / "nix");
    }
    return result;
}

std::filesystem::path getDataDir()
{
    auto dir = getEnvOs(OS_STR("NIX_DATA_HOME"));
    if (dir)
        return *dir;
    return unix::xdg::getDataHome() / "nix";
}

std::filesystem::path getStateDir()
{
    auto dir = getEnvOs(OS_STR("NIX_STATE_HOME"));
    if (dir)
        return *dir;
    return unix::xdg::getStateHome() / "nix";
}

std::filesystem::path createNixStateDir()
{
    std::filesystem::path dir = getStateDir();
    createDirs(dir);
    return dir;
}

std::string expandTilde(std::string_view path)
{
    // TODO: expand ~user ?
    auto tilde = path.substr(0, 2);
    if (tilde == "~/" || tilde == "~") {
        auto suffix = path.size() >= 2 ? std::string(path.substr(2)) : std::string{};
        return (getHome() / suffix).string();
    } else
        return std::string(path);
}

} // namespace nix
