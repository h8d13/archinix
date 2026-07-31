#pragma once
/**
 * @file
 *
 * @brief SourcePath
 */

#include "nix/util/ref.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/source-accessor.hh"

namespace nix {

/**
 * An abstraction for accessing source files during
 * evaluation. Currently, it's just a wrapper around `CanonPath` that
 * accesses files in the regular filesystem, but in the future it will
 * support fetching files in other ways.
 */
struct SourcePath
{
    ref<SourceAccessor> accessor;
    CanonPath path;

    SourcePath(ref<SourceAccessor> accessor, CanonPath path = CanonPath::root)
        : accessor(std::move(accessor))
        , path(std::move(path))
    {
    }

    /**
     * Stream this `SourcePath`'s contents into `sink`; it must denote a
     * regular file. Flat serialisation (`dumpPath` below) is this plus
     * nothing.
     */
    void readFile(Sink & sink, fun<void(uint64_t)> sizeCallback = [](uint64_t size) {}) const
    {
        return accessor->readFile(path, sink, std::move(sizeCallback));
    }

    /**
     * Dump this `SourcePath` to `sink` as a NAR archive.
     */
    void dumpPath(Sink & sink) const;

    std::string to_string() const;
};

std::ostream & operator<<(std::ostream & str, const SourcePath & path);

} // namespace nix
