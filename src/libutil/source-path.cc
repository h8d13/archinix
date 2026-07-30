#include "nix/util/source-path.hh"

namespace nix {

void SourcePath::dumpPath(Sink & sink, PathFilter & filter) const
{
    return accessor->dumpPath(path, sink, filter);
}

std::string SourcePath::to_string() const
{
    return accessor->showPath(path);
}

std::ostream & operator<<(std::ostream & str, const SourcePath & path)
{
    str << path.to_string();
    return str;
}

} // namespace nix
