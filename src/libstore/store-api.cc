#include "nix/util/logging.hh"
#include "nix/util/source-accessor.hh"
#include "nix/store/globals.hh"
#include "nix/store/store-api.hh"
#include "nix/store/store-open.hh"
#include "nix/util/util.hh"
#include "nix/util/thread-pool.hh"
#include "nix/util/archive.hh"
#include "nix/util/callback.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/signals.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/file-system.hh"

#include "store-config-private.hh"

#include <filesystem>
#include <nlohmann/json.hpp>


using json = nlohmann::json;

namespace nix {

void StoreConfig::anchor() {}

void InvalidPath::anchor() {}

void Unsupported::anchor() {}

void SubstituteGone::anchor() {}

void SubstituterDisabled::anchor() {}

void StoreConfigBase::anchor() {}

static std::string canonStoreDir(std::string path)
{
    if (path.empty() || path[0] != '/')
        throw UsageError("store directory path \"%s\" is not an absolute path", path);
    return CanonPath(std::move(path)).abs();
}

static std::string canonStoreDir(std::filesystem::path path)
{
    if (!path.is_absolute())
        throw UsageError("store directory path %s is not an absolute path", PathFmt(path));
    return canonPath(std::move(path)).string();
}

StoreConfigBase::StoreDirSetting::StoreDirSetting(Config * options, FilePathType pathType)
    : BaseSetting<std::string>(
          [pathType]() -> std::string {
              auto envOverrides = getEnvOsNonEmpty(OS_STR("NIX_STORE_DIR")).or_else([] {
                  return getEnvOsNonEmpty(OS_STR("NIX_STORE"));
              });

              switch (pathType) {
              case FilePathType::Unix:
                  return canonStoreDir(envOverrides.transform([](const auto & s) { return os_string_to_string(s); })
                                           .value_or(NIX_STORE_DIR));

              case FilePathType::Native:
                  return canonStoreDir(envOverrides.transform([](const auto & s) { return std::filesystem::path(s); })
                                           .or_else([]() -> std::optional<std::filesystem::path> {
                                               return std::filesystem::path{NIX_STORE_DIR};
                                           })
                                           .value());
              }
              assert(false);
          }(),
          true,
          "store",
          R"(
            Logical location of the Nix store, usually `/nix/store`.

            Defaults to [`NIX_STORE_DIR`](@docroot@/command-ref/env-common.md#env-NIX_STORE_DIR) if unset.

            Note that you can only copy store paths between stores if they have the same `store` setting.
          )",
          {})
    , pathType(pathType)
{
    options->addSetting(this);
}

std::string StoreConfigBase::StoreDirSetting::parse(const std::string & str) const
{
    if (str.empty())
        throw UsageError("setting '%s' is a path and paths cannot be empty", name);

    switch (pathType) {
    case FilePathType::Unix:
        return canonStoreDir(str);
    case FilePathType::Native:
        return canonStoreDir(std::filesystem::path(str));
    }
    assert(false);
}

StoreConfigBase::StoreConfigBase(const StringMap & params, FilePathType pathType)
    : Config(params)
    , storeDir_{this, pathType}
{
}

StoreConfig::StoreConfig(const Params & params, FilePathType pathType)
    : StoreConfigBase(params, pathType)
    , StoreDirConfig{storeDir_}
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

std::filesystem::path Store::followLinksToStore(std::string_view _path) const
{
    auto path = absPath(std::string(_path));

    // Limit symlink follows to prevent infinite loops
    unsigned int followCount = 0;
    const unsigned int maxFollow = 1024;

    while (!isInStore(path.string())) {
        if (!std::filesystem::is_symlink(path))
            break;

        if (++followCount >= maxFollow)
            throw Error("too many symbolic links encountered while resolving '%s'", _path);

        auto target = readLink(path);
        auto parentPath = path.parent_path();
        path = absPath(target, &parentPath);
    }

    if (!isInStore(path.string()))
        throw BadStorePath("path %1% is not in the Nix store", PathFmt(path));
    return path;
}

StorePath Store::followLinksToStorePath(std::string_view path) const
{
    return toStorePath(followLinksToStore(path).string()).first;
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
        if (settings.warnLargePathThreshold && lengthSource.total >= settings.warnLargePathThreshold)
            warn("copied large path '%s' to the store (%s)", path, renderSize(lengthSource.total));
    });
    dumpPath(path, *sink, fsm, filter);
    sink->finish();
    return storePath.value();
}


/*
The aim of this function is to compute in one pass the correct ValidPathInfo for
the files that we are trying to add to the store. To accomplish that in one
pass, given the different kind of inputs that we can take (normal nar archives,
nar archives with non SHA-256 hashes, and flat files), we set up a net of sinks
and aliases. Also, since the dataflow is obfuscated by this, we include here a
graphviz diagram:

digraph graphname {
    node [shape=box]
    fileSource -> narSink
    narSink [style=dashed]
    narSink -> unusualHashTee [style = dashed, label = "Recursive && !SHA-256"]
    narSink -> narHashSink [style = dashed, label = "else"]
    unusualHashTee -> narHashSink
    unusualHashTee -> caHashSink
    fileSource -> parseSink
    parseSink [style=dashed]
    parseSink-> fileSink [style = dashed, label = "Flat"]
    parseSink -> blank [style = dashed, label = "Recursive"]
    fileSink -> caHashSink
}
*/
ValidPathInfo Store::addToStoreSlow(
    std::string_view name,
    const SourcePath & srcPath,
    ContentAddressMethod method,
    HashAlgorithm hashAlgo,
    const StorePathSet & references,
    std::optional<Hash> expectedCAHash)
{
    HashSink narHashSink{HashAlgorithm::SHA256};
    HashSink caHashSink{hashAlgo};

    /* Note that fileSink and unusualHashTee must be mutually exclusive, since
       they both write to caHashSink. Note that that requisite is currently true
       because the former is only used in the flat case. */
    RegularFileSink fileSink{caHashSink};
    TeeSink unusualHashTee{narHashSink, caHashSink};

    auto & narSink = method == ContentAddressMethod::Raw::NixArchive && hashAlgo != HashAlgorithm::SHA256
                         ? static_cast<Sink &>(unusualHashTee)
                         : narHashSink;

    /* Functionally, this means that fileSource will yield the content of
       srcPath. The fact that we use scratchpadSink as a temporary buffer here
       is an implementation detail. */
    auto fileSource = sinkToSource([&](Sink & scratchpadSink) { srcPath.dumpPath(scratchpadSink); });

    /* tapped provides the same data as fileSource, but we also write all the
       information to narSink. */
    TeeSource tapped{*fileSource, narSink};

    NullFileSystemObjectSink blank;
    auto & parseSink = method.getFileIngestionMethod() == FileIngestionMethod::Flat
                           ? (FileSystemObjectSink &) fileSink
                           : (FileSystemObjectSink &) blank; // for recursive we do recursive

    /* The information that flows from tapped (besides being replicated in
       narSink), is now put in parseSink. */
    parseDump(parseSink, tapped);

    /* We extract the result of the computation from the sink by calling
       finish. */
    auto [narHash, narSize] = narHashSink.finish();

    auto hash = method == ContentAddressMethod::Raw::NixArchive && hashAlgo == HashAlgorithm::SHA256
                    ? narHash
                    : caHashSink.finish().hash;

    if (expectedCAHash && expectedCAHash != hash)
        throw Error("hash mismatch for '%s'", srcPath);

    auto info = ValidPathInfo::makeFromCA(
        *this,
        name,
        ContentAddressWithReferences::fromParts(
            method,
            hash,
            {
                .others = references,
                .self = false,
            }),
        narHash);
    info.narSize = narSize;

    if (!isValidPath(info.path)) {
        auto source = sinkToSource([&](Sink & scratchpadSink) { srcPath.dumpPath(scratchpadSink); });
        addToStore(info, *source);
    }

    return info;
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
    , pathInfoCache(make_ref<decltype(pathInfoCache)::element_type>((size_t) config.pathInfoCacheSize))
{
    assertLibStoreInitialized();
}

bool StoreConfig::getReadOnly() const
{
    return settings.readOnlyMode;
}

bool Store::PathInfoCacheValue::isKnownNow()
{
    /* no substituter TTLs: cached local path info never expires */
    return true;
}

void Store::invalidatePathInfoCacheFor(const StorePath & path)
{
    pathInfoCache->lock()->erase(path);
}






bool Store::isValidPath(const StorePath & storePath)
{
    auto res = pathInfoCache->lock()->get(storePath);
    if (res && res->isKnownNow()) {
        stats.narInfoReadAverted++;
        return res->didExist();
    }

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

ref<const ValidPathInfo> Store::queryPathInfo(const StorePath & storePath)
{
    std::promise<ref<const ValidPathInfo>> promise;

    queryPathInfo(storePath, {[&](std::future<ref<const ValidPathInfo>> result) {
                      try {
                          promise.set_value(result.get());
                      } catch (...) {
                          promise.set_exception(std::current_exception());
                      }
                  }});

    return promise.get_future().get();
}

static bool goodStorePath(const StorePath & expected, const StorePath & actual)
{
    return expected.hashPart() == actual.hashPart()
           && (expected.name() == Store::MissingName || expected.name() == actual.name());
}

std::optional<std::shared_ptr<const ValidPathInfo>> Store::queryPathInfoFromClientCache(const StorePath & storePath)
{
    auto hashPart = std::string(storePath.hashPart());

    auto res = pathInfoCache->lock()->get(storePath);
    if (res && res->isKnownNow()) {
        stats.narInfoReadAverted++;
        if (res->didExist())
            return std::make_optional(res->value);
        else
            return std::make_optional(nullptr);
    }

    return std::nullopt;
}

void Store::queryPathInfo(const StorePath & storePath, Callback<ref<const ValidPathInfo>> callback) noexcept
{
    auto hashPart = std::string(storePath.hashPart());

    try {
        auto r = queryPathInfoFromClientCache(storePath);
        if (r.has_value()) {
            std::shared_ptr<const ValidPathInfo> & info = *r;
            if (info)
                return callback(ref(info));
            else
                throw InvalidPath("path '%s' is not valid", printStorePath(storePath));
        }
    } catch (...) {
        return callback.rethrow();
    }

    auto callbackPtr = std::make_shared<decltype(callback)>(std::move(callback));

    queryPathInfoUncached(
        storePath, {[this, storePath, hashPart, callbackPtr](std::future<std::shared_ptr<const ValidPathInfo>> fut) {
            try {
                auto info = fut.get();

                pathInfoCache->lock()->upsert(storePath, PathInfoCacheValue{.value = info});

                if (!info || !goodStorePath(storePath, info->path)) {
                    stats.narInfoMissing++;
                    throw InvalidPath("path '%s' is not valid", printStorePath(storePath));
                }

                (*callbackPtr)(ref<const ValidPathInfo>(info));
            } catch (...) {
                callbackPtr->rethrow();
            }
        }});
}





/* Return a string accepted by decodeValidPathInfo() that
   registers the specified paths as valid.  Note: it's the
   responsibility of the caller to provide a closure. */
std::string Store::makeValidityRegistration(const StorePathSet & paths, bool showDerivers, bool showHash)
{
    std::string s = "";

    for (auto & i : paths) {
        s += printStorePath(i) + "\n";

        auto info = queryPathInfo(i);

        if (showHash) {
            s += info->narHash.to_string(HashFormat::Base16, false) + "\n";
            s += fmt("%1%\n", info->narSize);
        }

        auto deriver = showDerivers && info->deriver ? printStorePath(*info->deriver) : "";
        s += deriver + "\n";

        s += fmt("%1%\n", info->references.size());

        for (auto & j : info->references)
            s += printStorePath(j) + "\n";
    }

    return s;
}

StorePathSet Store::exportReferences(const StorePathSet & storePaths, const StorePathSet & inputPaths)
{
    StorePathSet paths;

    for (auto & storePath : storePaths) {
        if (!inputPaths.count(storePath))
            throw Error(
                "cannot export references of path '%s' because it is not in the input closure of the derivation",
                printStorePath(storePath));

        computeFSClosure({storePath}, paths);
    }

    return paths;
}

const Store::Stats & Store::getStats()
{
    stats.pathInfoCacheSize = pathInfoCache->readLock()->size();
    return stats;
}








const std::filesystem::path & StoreConfig::getStateDir() const
{
    return settings.nixStateDir;
}

const std::filesystem::path & StoreConfig::getLogDir() const
{
    static std::filesystem::path logDir = [] {
        return getEnvOsNonEmpty(OS_STR("NIX_LOG_DIR"))
            .transform([](auto && s) { return std::filesystem::path(s); })
            .or_else([]() -> std::optional<std::filesystem::path> {
                return NIX_LOG_DIR;
            })
            .transform([](auto && s) { return canonPath(s); })
            .value();
    }();
    return logDir;
}

} // namespace nix
