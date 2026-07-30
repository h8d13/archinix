#include "nix/util/logging.hh"
#include "nix/util/source-accessor.hh"
#include "nix/store/globals.hh"
#include "nix/store/local-store.hh"
#include "nix/store/store-open.hh"
#include "nix/util/util.hh"
#include "nix/util/archive.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/signals.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/file-system.hh"

#include "store-config-private.hh"

#include <filesystem>



namespace nix {

void InvalidPath::anchor() {}

bool StoreDirConfig::isInStore(std::string_view path) const
{
    return isInDir(path, storeDir);
}

std::pair<StorePath, CanonPath> StoreDirConfig::toStorePath(std::string_view path) const
{
    if (!isInStore(path))
        throw Error("path '%1%' is not in the Nix store", path);
    auto slash = path.find('/', storeDir.size() + 1);
    if (slash == std::string::npos)
        return {parseStorePath(path), CanonPath::root};
    else
        return {parseStorePath(path.substr(0, slash)), CanonPath{path.substr(slash)}};
}

StorePath LocalStore::addToStore(std::string_view name, const SourcePath & path)
{
    std::optional<StorePath> storePath;
    auto sink = sourceToSink([&](Source & source) {
        LengthSource lengthSource(source);
        storePath = addToStoreFromDump(lengthSource, name);
        if (config->warnLargePathThreshold && lengthSource.total >= config->warnLargePathThreshold)
            warn("copied large path '%s' to the store (%s)", path, renderSize(lengthSource.total));
    });
    path.dumpPath(*sink);
    sink->finish();
    return storePath.value();
}





static bool goodStorePath(const StorePath & expected, const StorePath & actual)
{
    return expected.hashPart() == actual.hashPart()
           && (expected.name() == LocalStore::MissingName || expected.name() == actual.name());
}

ref<const ValidPathInfo> LocalStore::queryPathInfo(const StorePath & storePath)
{
    auto info = queryPathInfoUnchecked(storePath);

    if (!info || !goodStorePath(storePath, info->path))
        throw InvalidPath("path '%s' is not valid", printStorePath(storePath));

    return ref<const ValidPathInfo>(info);
}

} // namespace nix
