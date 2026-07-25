#pragma once
///@file

#include "nix/store/path.hh"
#include "nix/util/hash.hh"
#include "nix/store/content-address.hh"
#include "nix/util/serialise.hh"
#include "nix/util/sync.hh"
#include "nix/store/path-info.hh"
#include "nix/util/repair-flag.hh"
#include "nix/store/store-dir-config.hh"
#include "nix/util/source-path.hh"

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <chrono>

namespace nix {

MakeError(InvalidPath, Error);
MakeError(Unsupported, Error);
MakeError(SubstituteGone, Error);
MakeError(SubstituterDisabled, Error);


struct SourceAccessor;
class Store;

typedef std::map<std::string, StorePath> OutputPathMap;

enum BuildMode : uint8_t { bmNormal, bmRepair, bmCheck };

enum TrustedFlag : bool { NotTrusted = false, Trusted = true };

typedef std::map<StorePath, std::optional<ContentAddress>> StorePathCAMap;

/**
 * Exactly one store type exists (the local store). A store consists of
 * a config struct holding plain fields, and a `Store` subclass holding
 * the implementation; the config's `openStore()` builds the latter.
 */
struct StoreConfig : public StoreDirConfig
{
private:
    /* VTable anchor: avoids a weak vtable, which breaks dynamic_cast
       across shared libraries. Root of the config hierarchy now that
       the Config base is gone. */
    virtual void anchor();

public:
    /**
     * The logical store directory (`/nix/store`). Baked in: the
     * initramfs and GRUB entries spell it out, so it cannot move.
     */
    static const std::string & logicalStoreDir();

    StoreConfig();

    virtual ~StoreConfig() {}

    /**
     * Warn when adding a path larger than this many bytes (by NAR
     * size). 0 disables the warning.
     */
    uint64_t warnLargePathThreshold = 0;

    /**
     * @return The state directory for this store.
     */
    virtual const std::filesystem::path & getStateDir() const = 0;

    /**
     * Open a store of the type corresponding to this configuration
     * type.
     */
    virtual ref<Store> openStore() const = 0;

    /**
     * Get a textual representation of the store.
     *
     * @warning This is only suitable for logging or error messages.
     * Must NOT be used as a cache key or otherwise be relied upon to
     * be stable.
     */
    virtual std::string getHumanReadableURI() const
    {
        return "local";
    }
};

/**
 * A Store (client)
 *
 * This is an interface type allowing for create and read operations on
 * a collection of store objects, and also building new store objects
 * from `Derivation`s. See the manual for further details.
 *
 * "client" used is because this is just one view/actor onto an
 * underlying resource, which could be an external process (daemon
 * server), file system state, etc.
 */
class Store : public std::enable_shared_from_this<Store>, public StoreDirConfig
{
    /* VTable anchor to avoid weak linkage of the vtable - it breaks
       dynamic_cast across shared libraries on Darwin. */
    virtual void anchor() = 0;

public:

    using Config = StoreConfig;

    const Config & config;

    /**
     * @note Avoid churn, since we used to inherit from `Config`.
     */
    operator const Config &() const
    {
        return config;
    }

protected:



    Store(const Store::Config & config);

public:
    /**
     * Perform any necessary effectful operation to make the store up and
     * running
     */
    virtual void init() {};

    virtual ~Store() {}

    /**
     * Check whether a path is valid.
     */
    bool isValidPath(const StorePath & path);

protected:

    virtual bool isValidPathUncached(const StorePath & path);

public:

    /**
     * If requested, substitute missing paths. This
     * implements nix-copy-closure's --use-substitutes
     * flag.
     */


    /**
     * Query the set of all valid paths. Note that for some store
     * backends, the name part of store paths may be replaced by `x`
     * (i.e. you'll get `/nix/store/<hash>-x` rather than
     * `/nix/store/<hash>-<name>`). Use queryPathInfo() to obtain the
     * full store path. FIXME: should return a set of
     * `std::variant<StorePath, HashPart>` to get rid of this hack.
     */
    virtual StorePathSet queryAllValidPaths()
    {
        unsupported("queryAllValidPaths");
    }

    constexpr static const char * MissingName = "x";

    /**
     * Query information about a valid path. It is permitted to omit
     * the name part of the store path.
     */
    ref<const ValidPathInfo> queryPathInfo(const StorePath & path);

protected:

    /**
     * @return null if the path is not valid in this store.
     */
    virtual std::shared_ptr<const ValidPathInfo> queryPathInfoUncached(const StorePath & path) = 0;

public:

    /**
     * Queries the set of incoming FS references for a store path.
     * The result is not cleared.
     */
    virtual void queryReferrers(const StorePath & path, StorePathSet & referrers)
    {
        unsupported("queryReferrers");
    }






    /**
     * Import a path into the store. Note that the entire NAR may not be read from `narSource`, e.g. if the path is
     * already valid.
     */
    virtual void addToStore(const ValidPathInfo & info, Source & narSource, RepairFlag repair = NoRepair) = 0;


    /**
     * Copy the contents of a path to the store and register the
     * validity the resulting path.
     *
     * @return The resulting path is returned.
     * @param filter This function can be used to exclude files (see
     * libutil/archive.hh).
     */
    virtual StorePath addToStore(
        std::string_view name,
        const SourcePath & path,
        ContentAddressMethod method = ContentAddressMethod::Raw::NixArchive,
        HashAlgorithm hashAlgo = HashAlgorithm::SHA256,
        const StorePathSet & references = StorePathSet(),
        PathFilter & filter = defaultPathFilter,
        RepairFlag repair = NoRepair);

    /**
     * Like addToStore(), but the contents of the path are contained
     * in `dump`, which is either a NAR serialisation (if recursive ==
     * true) or simply the contents of a regular file (if recursive ==
     * false).
     *
     * `dump` may be drained.
     *
     * @param dumpMethod What serialisation format is `dump`, i.e. how
     * to deserialize it. Must either match hashMethod or be
     * `FileSerialisationMethod::NixArchive`.
     *
     * @param hashMethod How content addressing? Need not match be the
     * same as `dumpMethod`.
     *
     * @todo remove?
     */
    virtual StorePath addToStoreFromDump(
        Source & dump,
        std::string_view name,
        FileSerialisationMethod dumpMethod = FileSerialisationMethod::NixArchive,
        ContentAddressMethod hashMethod = ContentAddressMethod::Raw::NixArchive,
        HashAlgorithm hashAlgo = HashAlgorithm::SHA256,
        const StorePathSet & references = StorePathSet(),
        RepairFlag repair = NoRepair) = 0;



    /**
     * Write a NAR dump of a store path.
     */
    virtual void narFromPath(const StorePath & path, Sink & sink);






    /**
     * Check the integrity of the Nix store.
     *
     * @return true if errors remain.
     */
    virtual bool verifyStore(bool checkContents, RepairFlag repair = NoRepair)
    {
        return false;
    };

    /**
     * @return An object to access files in the Nix store, across all
     * store objects.
     */
    virtual ref<SourceAccessor> getFSAccessor(bool requireValidPath = true) = 0;

    /**
     * @return An object to access files for a specific store object in
     * the Nix store.
     *
     * @return nullptr if the store doesn't contain an object at the
     * given path.
     */
    virtual std::shared_ptr<SourceAccessor> getFSAccessor(const StorePath & path, bool requireValidPath = true) = 0;

    /**
     * Get an accessor for the store object or throw an Error if it's invalid or
     * doesn't exist.
     *
     * @throws InvalidPath if the store object doesn't exist or (if requireValidPath = true) is
     * invalid.
     */
    [[nodiscard]] ref<SourceAccessor> requireStoreObjectAccessor(const StorePath & path, bool requireValidPath = true)
    {
        auto accessor = getFSAccessor(path, requireValidPath);
        if (!accessor) {
            throw InvalidPath(
                requireValidPath ? "path '%1%' is not a valid store path" : "store path '%1%' does not exist",
                printStorePath(path));
        }
        return ref<SourceAccessor>{accessor};
    }



    /* Utility functions. */



    /**
     * Sort a set of paths topologically under the references
     * relation.  If p refers to q, then p precedes q in this list.
     * Virtual to allow for more efficient implementations in derived classes.
     */
    virtual StorePaths topoSortPaths(const StorePathSet & paths);


    /**
     * Establish a connection to the store, for store types that have
     * a notion of connection. Otherwise this is a no-op.
     */
    virtual void connect() {};

    /**
     * Get the protocol version of this store or it's connection.
     */
    virtual unsigned int getProtocol()
    {
        return 0;
    };

    /**
     * @return/ whether store trusts *us*.
     *
     * `std::nullopt` means we do not know.
     *
     * @note This is the opposite of the StoreConfig::isTrusted
     * store setting. That is about whether *we* trust the store.
     */
    /**
     * Synchronises the options of the client with those of the daemon
     * (a no-op when there’s no daemon)
     */
    virtual void setOptions() {}

    virtual std::optional<std::string> getVersion()
    {
        return {};
    }

protected:

    /**
     * Helper for methods that are not unsupported: this is used for
     * default definitions for virtual methods that are meant to be overridden.
     *
     * @todo Using this should be a last resort. It is better to make
     * the method "virtual pure" and/or move it to a subclass.
     */
    [[noreturn]] void unsupported(const std::string & op)
    {
        throw Unsupported("operation '%s' is not supported by store '%s'", op, config.getHumanReadableURI());
    }
};




} // namespace nix
