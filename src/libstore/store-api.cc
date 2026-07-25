#include "nix/util/logging.hh"
#include "nix/util/source-accessor.hh"
#include "nix/store/globals.hh"
#include "nix/store/store-api.hh"
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

void StoreConfig::anchor() {}

void InvalidPath::anchor() {}

void Unsupported::anchor() {}

void SubstituteGone::anchor() {}

void SubstituterDisabled::anchor() {}

/* The logical store dir is baked in: the initramfs and GRUB entries
   spell out /nix/store, so only the *physical* half (realStoreDir) can
   move with the root. Kept as a function-local static so it outlives
   every config that references it. */
const std::string & StoreConfig::logicalStoreDir()
{
    static const std::string dir = [] {
        std::filesystem::path p{NIX_STORE_DIR};
        if (!p.is_absolute())
            throw UsageError("store directory path %s is not an absolute path", PathFmt(p));
        return canonPath(std::move(p)).string();
    }();
    return dir;
}

StoreConfig::StoreConfig()
    : StoreDirConfig{logicalStoreDir()}
{
}

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

StorePath Store::addToStore(
    std::string_view name,
    const SourcePath & path,
    ContentAddressMethod method,
    HashAlgorithm hashAlgo,
    const StorePathSet & references,
    PathFilter & filter,
    RepairFlag repair)
{
    FileSerialisationMethod fsm;
    switch (method.getFileIngestionMethod()) {
    case FileIngestionMethod::Flat:
        fsm = FileSerialisationMethod::Flat;
        break;
    case FileIngestionMethod::NixArchive:
        fsm = FileSerialisationMethod::NixArchive;
        break;
    }
    std::optional<StorePath> storePath;
    auto sink = sourceToSink([&](Source & source) {
        LengthSource lengthSource(source);
        storePath = addToStoreFromDump(lengthSource, name, fsm, method, hashAlgo, references, repair);
        if (config.warnLargePathThreshold && lengthSource.total >= config.warnLargePathThreshold)
            warn("copied large path '%s' to the store (%s)", path, renderSize(lengthSource.total));
    });
    dumpPath(path, *sink, fsm, filter);
    sink->finish();
    return storePath.value();
}


void Store::narFromPath(const StorePath & path, Sink & sink)
{
    auto accessor = requireStoreObjectAccessor(path);
    SourcePath sourcePath{accessor};
    dumpPath(sourcePath, sink, FileSerialisationMethod::NixArchive);
}


Store::Store(const Store::Config & config)
    : StoreDirConfig{config}
    , config{config}
{
    assertLibStoreInitialized();
}






bool Store::isValidPath(const StorePath & storePath)
{
    return isValidPathUncached(storePath);
}

/* Default implementation for stores that only implement
   queryPathInfoUncached(). */
bool Store::isValidPathUncached(const StorePath & path)
{
    try {
        queryPathInfo(path);
        return true;
    } catch (InvalidPath &) {
        return false;
    }
}

static bool goodStorePath(const StorePath & expected, const StorePath & actual)
{
    return expected.hashPart() == actual.hashPart()
           && (expected.name() == Store::MissingName || expected.name() == actual.name());
}

ref<const ValidPathInfo> Store::queryPathInfo(const StorePath & storePath)
{
    auto info = queryPathInfoUncached(storePath);

    if (!info || !goodStorePath(storePath, info->path))
        throw InvalidPath("path '%s' is not valid", printStorePath(storePath));

    return ref<const ValidPathInfo>(info);
}

} // namespace nix
