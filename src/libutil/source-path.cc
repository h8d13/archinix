#include "nix/util/source-path.hh"

namespace nix {

void SourcePath::dumpPath(Sink & sink) const
{
    return accessor->dumpPath(path, sink);
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
