#include <atomic>
#include "nix/util/source-accessor.hh"

namespace nix {

void SourceAccessorError::anchor() {}

void FileNotFound::anchor() {}

void NotASymlink::anchor() {}

void NotADirectory::anchor() {}

void NotARegularFile::anchor() {}

void SymlinkNotAllowed::anchor() {}

static std::atomic<size_t> nextNumber{0};

SourceAccessor::SourceAccessor()
    : number(++nextNumber)
    , displayPrefix{"«unknown»"}
{
}

bool SourceAccessor::pathExists(const CanonPath & path)
{
    return maybeLstat(path).has_value();
}

SourceAccessor::Stat SourceAccessor::lstat(const CanonPath & path)
{
    if (auto st = maybeLstat(path))
        return *st;
    else
        throw FileNotFound("path '%s' does not exist", showPath(path));
}

void SourceAccessor::setPathDisplay(std::string displayPrefix, std::string displaySuffix)
{
    this->displayPrefix = std::move(displayPrefix);
    this->displaySuffix = std::move(displaySuffix);
}

std::string SourceAccessor::showPath(const CanonPath & path)
{
    return displayPrefix + path.abs() + displaySuffix;
}

} // namespace nix
