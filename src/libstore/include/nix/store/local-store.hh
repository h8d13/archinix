#pragma once
///@file

#include "nix/store/sqlite.hh"

#include "nix/store/pathlocks.hh"
#include "nix/store/store-api.hh"
#include "nix/store/local-fs-store.hh"
#include "nix/store/local-settings.hh"
#include "nix/util/sync.hh"

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <boost/unordered/concurrent_flat_set.hpp>

namespace nix {

class ThreadPool;

/**
 * Nix store and database schema version.
 *
 * Version 1 (or 0) was Nix <=
 * 0.7.  Version 2 was Nix 0.8 and 0.9.  Version 3 is Nix 0.10.
 * Version 4 is Nix 0.11.  Version 5 is Nix 0.12-0.16.  Version 6 is
 * Nix 1.0.  Version 7 is Nix 1.3. Version 10 is 2.0.
 */
const int nixSchemaVersion = 10;

struct OptimiseStats
{
    unsigned long filesLinked = 0;
    uint64_t bytesFreed = 0;
    /**
     * Link candidates walked (files and, where linkable, symlinks),
     * linked or not. Only the progress counter reads it: a node can
     * be skipped here for several reasons, and counting just the
     * links would stall at a fraction of the walk.
     */
    uint64_t filesVisited = 0;
};

struct LocalBuildStoreConfig : virtual LocalFSStoreConfig
{
private:
    void anchor() override;

public:
    /**
     * Per-store knobs. These used to be read off a global `settings`
     * singleton fed by nix.conf; they are now genuinely per-store,
     * which is what the old code said it wanted.
     */
    LocalSettings localSettings;

    const LocalSettings & getLocalSettings() const
    {
        return localSettings;
    }
};

struct LocalStoreConfig : std::enable_shared_from_this<LocalStoreConfig>,
                          virtual LocalFSStoreConfig,
                          virtual LocalBuildStoreConfig
{
    LocalStoreConfig(const std::filesystem::path & path);

private:
    void anchor() override;

public:
    /**
     * Open the store even though its database is on a read-only
     * filesystem: no locking, and SQLite opened `immutable`.
     */
    bool readOnly = false;

    /**
     * Warn rather than fail when the garbage collector cannot delete a
     * file.
     */
    bool ignoreGcDeleteFailure = false;

    static const std::string name()
    {
        return "Local Store";
    }

    ref<Store> openStore() const override;
};

MakeError(PathInUse, Error);

class LocalStore : public virtual LocalFSStore, public virtual GcStore
{
    void anchor() override;

public:

    using Config = LocalStoreConfig;

    ref<const LocalStoreConfig> config;

private:

    /**
     * Lock file used for upgrading.
     */
    AutoCloseFD globalLock;

    struct State
    {
        /**
         * The SQLite database object.
         */
        SQLite db;

        struct Stmts;
        std::unique_ptr<Stmts> stmts;

    };

    /**
     * Mutable state. It's behind a `ref` to reduce false sharing
     * between immutable and mutable fields.
     */
    ref<Sync<State>> _state;

public:

    const std::filesystem::path dbDir;
    const std::filesystem::path linksDir;
    const std::filesystem::path reservedPath;
    const std::filesystem::path schemaPath;

private:

public:

    /**
     * Hack for build-remote.cc.
     */
    PathSet locksHeld;

    /**
     * Initialise the local store, upgrading the schema if
     * necessary.
     */
    LocalStore(ref<const Config> params);

    ~LocalStore();

    /**
     * Implementations of abstract store API methods.
     */

    bool isValidPathUncached(const StorePath & path) override;


    /**
     * Every valid path whose hash part starts with `hashPrefix`. The
     * prefix may be shorter than the full 32 base-32 characters, which
     * is what makes the short ids `nixgen-listid` prints usable as
     * arguments. Returning the whole set rather than the first match
     * lets the caller distinguish "unique" from "ambiguous".
     *
     * The db is the source of truth: a directory listing also shows
     * `.links` and half-written `tmp-*` imports, which is exactly what
     * callers must not match against.
     */
    StorePathSet queryPathsByHashPrefix(const std::string & hashPrefix);

    StorePathSet queryAllValidPaths() override;

    std::shared_ptr<const ValidPathInfo> queryPathInfoUncached(const StorePath & path) override;

    void queryReferrers(const StorePath & path, StorePathSet & referrers) override;

    void addToStore(const ValidPathInfo & info, Source & source, RepairFlag repair) override;

    StorePath addToStoreFromDump(
        Source & dump,
        std::string_view name,
        FileSerialisationMethod dumpMethod,
        ContentAddressMethod hashMethod,
        HashAlgorithm hashAlgo,
        const StorePathSet & references,
        RepairFlag repair) override;

    /**
     * Per-file NAR hashes captured while restoring an import (the
     * bytes are hashed as they stream through the restore sink), so a
     * following optimisePath() can dedup without re-reading and
     * re-hashing every file. Keys are CanonPath::abs() strings
     * relative to the restored root.
     */
    struct ImportFileHashes
    {
        std::map<std::string, Hash> files;

        /**
         * Files replaced by a hard link into the link farm while the
         * import streamed (their content already existed in .links),
         * so their data never occupied disk. Cuts the peak-space cost
         * of importing a mostly-unchanged tree from "full un-deduped
         * snapshot" to "new content only".
         */
        uint64_t dedupedFiles = 0;
        uint64_t dedupedBytes = 0;

        /**
         * Symlinks restored. They carry no content to hash, so they
         * are absent from `files`, but optimisePath() hard-links them
         * like any other node where the platform allows it (and a
         * store tree is symlink-heavy), so a progress total built
         * from `files` alone would miss most of that pass.
         */
        uint64_t symlinks = 0;
    };

    StorePath addToStoreFromDump(
        Source & dump,
        std::string_view name,
        FileSerialisationMethod dumpMethod,
        ContentAddressMethod hashMethod,
        HashAlgorithm hashAlgo,
        const StorePathSet & references,
        RepairFlag repair,
        ImportFileHashes * fileHashes);

    /**
     * The global GC lock.
     */
    Sync<AutoCloseFD> _fdGCLock;

public:

    /**
     * The permanent root is just the user-facing symlink; roots here
     * live inside the scanned gcroots dir, so the historical
     * gcroots/auto indirection layer is gone.
     */
    std::filesystem::path
    addPermRoot(const StorePath & storePath, const std::filesystem::path & gcRoot) override;

private:

    void makeSymlink(const std::filesystem::path & link, const std::filesystem::path & target);

    AutoCloseFD openGCLock();

public:

    void collectGarbage(const GCOptions & options, GCResults & results) override;

    /**
     * Called by `collectGarbage` to trace in reverse.
     *
     * Using this rather than `queryReferrers` directly allows us to
     * fine-tune which referrers we consider for garbage collection;
     * some store implementations take advantage of this.
     */
    virtual void queryGCReferrers(const StorePath & path, StorePathSet & referrers)
    {
        return queryReferrers(path, referrers);
    }

    /**
     * Called by `collectGarbage` to recursively delete a path.
     * The default implementation simply calls `deletePath`, but it can be
     * overridden by stores that wish to provide their own deletion behaviour.
     *
     * @param isKnownPath true if this is a known store path, false if it's
     *        garbage/unknown content found in the store directory
     */
    virtual void deleteStorePath(const std::filesystem::path & path, uint64_t & bytesFreed, bool isKnownPath);

    /**
     * Optimise the disk space usage of the Nix store by hard-linking
     * files with the same contents.
     */
    void optimiseStore(OptimiseStats & stats);

    /**
     * Optimise a single store path. Optionally, test the encountered
     * symlinks for corruption.
     */
    void optimisePath(const std::filesystem::path & path, RepairFlag repair);

    /**
     * Optimise a single (typically freshly imported) store path: the
     * whole-store walk is not needed, older paths are already
     * farm-linked. fileHashes from addToStoreFromDump skips the
     * per-file content re-read.
     */
    void optimisePath(const StorePath & path, OptimiseStats & stats, const ImportFileHashes * fileHashes = nullptr);

    bool verifyStore(bool checkContents, RepairFlag repair) override;

protected:

    /**
     * Result of `verifyAllValidPaths`
     */
    struct VerificationResult
    {
        /**
         * Whether any errors were encountered
         */
        bool errors;

        /**
         * A set of so-far valid paths. The store objects pointed to by
         * those paths are suitable for further validation checking.
         */
        StorePathSet validPaths;
    };

    /**
     * First, unconditional step of `verifyStore`
     */
    virtual VerificationResult verifyAllValidPaths(RepairFlag repair);

public:

    /**
     * Register the validity of a path, i.e., that `path` exists, that
     * the paths referenced by it exists, and in the case of an output
     * path of a derivation, that it has been produced by a successful
     * execution of the derivation (or something equivalent).  Also
     * register the hash of the file system contents of the path.  The
     * hash must be a SHA-256 hash.
     */
    void registerValidPath(const ValidPathInfo & info);

    virtual void registerValidPaths(const ValidPathInfos & infos);



    std::optional<std::string> getVersion() override;

protected:

    void verifyPath(
        const StorePath & path,
        fun<bool(const StorePath &)> existsInStoreDir,
        StorePathSet & done,
        StorePathSet & validPaths,
        RepairFlag repair,
        bool & errors);

private:

    /**
     * Retrieve the current version of the database schema.
     * If the database does not exist yet, the version returned will be 0.
     */
    int getSchema();

    void openDB(State & state, bool create);

    /**
     * Perform or check if a database schema upgrade is needed.
     * @param dryRun only check if an upgrade is needed.
     * @return true if an upgrade is needed or was performed, false otherwise.
     */
    bool upgradeDBSchema(State & state, bool dryRun);

    void requireWritableStore();

    uint64_t queryValidPathId(State & state, const StorePath & path);

    uint64_t addValidPath(State & state, const ValidPathInfo & info);

    void invalidatePath(State & state, const StorePath & path);

    /**
     * Delete a path from the Nix store.
     */
    void invalidatePathChecked(const StorePath & path);

    std::shared_ptr<const ValidPathInfo> queryPathInfoInternal(State & state, const StorePath & path);

    void updatePathInfo(State & state, const ValidPathInfo & info);

    void findRoots(const std::filesystem::path & path, std::filesystem::file_type type, Roots & roots);

    void findRootsNoTemp(Roots & roots);


    std::pair<std::filesystem::path, AutoCloseFD> createTempDirInStore();

    /* Concurrent: shared by the optimiseStore worker threads. */
    typedef boost::concurrent_flat_set<ino_t> InodeHash;

    /* Whole-store optimise runs race on directory permission toggling;
       one run at a time per process (workers parallelise inside). */
    std::mutex optimiseStoreLock;

    /**
     * Everything one optimise run shares across its threads. The
     * counters are atomic rather than per-task-and-merged: a subtree
     * fan-out has no single merge point the way a path-at-a-time loop
     * did, and three relaxed increments per file are free next to the
     * two journaled metadata writes a link costs. `total` is a
     * constant of the run that optimisePath_ used to recompute from
     * `fileHashes` once per file.
     */
    struct OptimiseCtx
    {
        InodeHash & inodeHash;
        const ImportFileHashes * fileHashes = nullptr;
        size_t fileHashesBase = 0;
        uint64_t total = 0;
        std::atomic<uint64_t> visited{0};
        std::atomic<unsigned long> filesLinked{0};
        std::atomic<uint64_t> bytesFreed{0};

        OptimiseCtx(InodeHash & inodeHash)
            : inodeHash(inodeHash)
        {
        }

        void into(OptimiseStats & stats) const
        {
            stats.filesLinked += filesLinked;
            stats.bytesFreed += bytesFreed;
            stats.filesVisited += visited;
        }
    };

    /* name plus "is it a directory", so the parallel walk can tell
       subtrees (a task each) from files (the parent's own work)
       without an extra lstat; d_type is free from readdir. */
    struct OptimiseEnt
    {
        std::string name;
        bool isDir;
    };

    InodeHash loadInodeHash();
    std::vector<OptimiseEnt>
    readDirectoryIgnoringInodes(const std::filesystem::path & path, const InodeHash & inodeHash);
    void optimisePath_(
        Activity * act,
        OptimiseCtx & ctx,
        const std::filesystem::path & path,
        RepairFlag repair,
        bool * parentToggled = nullptr,
        ThreadPool * pool = nullptr);

    // Internal versions that are not wrapped in retry_sqlite.
    bool isValidPath_(State & state, const StorePath & path);
    void queryReferrers(State & state, const StorePath & path, StorePathSet & referrers);


    /* Only used for createTempDirInStore. */
    friend class DerivationBuilderImpl;
};

} // namespace nix
