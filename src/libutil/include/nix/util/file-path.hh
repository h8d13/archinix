#pragma once
///@file

#include <filesystem>

#include "nix/util/types.hh"

namespace nix {

/**
 * Paths are just `std::filesystem::path`s.
 */
typedef std::list<std::filesystem::path> Paths;
typedef std::set<std::filesystem::path> PathSet;

/**
 * Stop gap until `std::filesystem::path_view` from P1030R6 exists in a
 * future C++ standard.
 */
struct PathView : std::string_view
{
    using string_view = std::string_view;

    using string_view::string_view;

    PathView(const std::filesystem::path & path)
        : std::string_view{path.native()}
    {
    }

    PathView(const std::string & path)
        : std::string_view{path}
    {
    }

    const string_view & native() const
    {
        return *this;
    }

    string_view & native()
    {
        return *this;
    }
};

} // namespace nix
