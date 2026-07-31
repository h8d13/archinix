#pragma once
///@file

#include "nix/store/sqlite.hh"

#include "nix/store/pathlocks.hh"
#include "nix/store/store-api.hh"
#include "nix/store/gc-store.hh"
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
class LocalStore;

/**
 * The schema this store creates and the only one it opens. Written to
 * `<state>/db/schema` when the store is minted, compared on every open:
 * a store carrying anything else was made by something else. Bump it
 * when `schema.sql` changes and rebuild the stores.
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

/**
 * The store's configuration: plain fields, filled in from the root
 * path. One store type means one config type, so nothing here is
 * virtual and nothing is settable through a URI: arch/ owns the layout
 * above this point, and store knobs are defaults in `LocalSettings`
 * that you change by editing and recompiling.
 */
struct LocalStoreConfig : std::enable_shared_from_this<LocalStoreConfig>, StoreDirConfig
{
    /**
     * The store is opened with a root path and nothing else:
     * `stateDir` and `realStoreDir` are derived from it.
     */
    LocalStoreConfig(const std::filesystem::path & rootDir);

    /**
     * The logical store directory (`/nix/store`). Baked in: the
     * initramfs and GRUB entries spell it out, so it cannot move.
     */
    static const std::string & logicalStoreDir();

    /**
     * Directory prefixed to all other paths.
     */
    std::filesystem::path rootDir;

    /**
     * Directory where the store keeps its state (`<root>/nix/var/nix`).
     */
    std::filesystem::path stateDir;

    /**
     * Physical path of the store (`<root>/nix/store`). The *logical*
     * store path stays `storeDir`; only the physical half moves with
     * the root.
     */
    std::filesystem::path realStoreDir;

    /**
     * Warn when adding a path larger than this many bytes (by NAR
     * size). 0 disables the warning.
     */
    uint64_t warnLargePathThreshold = 0;

    /**
     * Per-store knobs. These used to be read off a global `settings`
     * singleton fed by nix.conf; they are now genuinely per-store,
     * which is what the old code said it wanted.
     */
    LocalSettings localSettings;

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

    const LocalSettings & getLocalSettings() const
    {
        return localSettings;
    }

    ref<LocalStore> openStore() const;
};

MakeError(PathInUse, Error);

/**
 * The store. Local filesystem, SQLite database, hard-link farm: the
 * only store this extraction has, so the class is concrete and its
 * methods are not virtual (upstream's Store/LocalFSStore/GcStore
 * interface split existed to hold remote and chroot stores as well).
 */
class LocalStore : public std::enable_shared_from_this<LocalStore>, public StoreDirConfig
{
public:

    using Config = LocalStoreConfig;

    ref<const LocalStoreConfig> config;

private:

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

    /**
     * Open the local store, creating its skeleton and database if the
     * root does not hold one yet.
     */
    LocalStore(const ref<const Config> & params);

    ~LocalStore();

    /**
     * Physical path of a store object.
     */
    std::filesystem::path toRealPath(const StorePath & storePath)
    {
        return config->realStoreDir / storePath.to_string();
    }

    /**
     * Check whether a path is valid.
     */
    bool isValidPath(const StorePath & path);

    /**
     * Query information about a valid path. It is permitted to omit
     * the name part of the store path.
     */
    ref<const ValidPathInfo> queryPathInfo(const StorePath & path);

    /**
     * Copy the contents of a path to the store and register the
     * validity of the resulting path.
     */
    StorePath addToStore(std::string_view name, const SourcePath & path);

    /**
     * Name part a store path may carry when only its hash is known.
     */
    constexpr static const char * MissingName = "x";



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

    StorePathSet queryAllValidPaths();

    void addToStore(const ValidPathInfo & info, Source & source);

    /**
     * Take a NAR stream into the store. The path it lands at follows
     * from its SHA-256 hash: the one ingestion route this store has,
     * so there is no method to pick and no references to declare.
     */
    StorePath addToStoreFromDump(Source & dump, std::string_view name);

    /**
     * Per-file NAR hashes captured while restoring an import (the
     * bytes are hashed as they stream through the restore sink), so a
     * following optimisePath() can dedup without re-reading and
     * re-hashing every file. Keys are CanonPath::abs() strings
     * relative to the restored root.
     */
    struct ImportFileHashes
    {
        /* transparent comparator: optimisePath looks entries up by a
           view into the path it already holds, without building a
           std::string per file to do it */
        std::map<std::string, Hash, std::less<>> files;

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

    StorePath
    addToStoreFromDump(Source & dump, std::string_view name, ImportFileHashes * fileHashes);

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
    addPermRoot(const StorePath & storePath, const std::filesystem::path & gcRoot);

private:

    void makeSymlink(const std::filesystem::path & link, const std::filesystem::path & target);

    AutoCloseFD openGCLock();

public:

    void collectGarbage(const GCOptions & options, GCResults & results);

    /**
     * Optimise the disk space usage of the Nix store by hard-linking
     * files with the same contents.
     */
    void optimiseStore(OptimiseStats & stats);

    /**
     * Optimise a single store path. Optionally, test the encountered
     * symlinks for corruption.
     */
    void optimisePath(const std::filesystem::path & path);

    /**
     * Optimise a single (typically freshly imported) store path: the
     * whole-store walk is not needed, older paths are already
     * farm-linked. fileHashes from addToStoreFromDump skips the
     * per-file content re-read.
     */
    void optimisePath(const StorePath & path, OptimiseStats & stats, const ImportFileHashes * fileHashes = nullptr);

    bool verifyStore(bool checkContents);

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
    VerificationResult verifyAllValidPaths();

    /**
     * Register the validity of a path: that `path` exists and that the
     * paths it references exist. Also records the SHA-256 hash of its
     * file system contents. Upstream's third clause, that an output
     * path was produced by a successful derivation build, has nothing
     * to assert here: nothing in this store builds.
     *
     * Not public: the two ingestion routes (`addToStore`,
     * `addToStoreFromDump`) call it, and a caller registering a path
     * the store did not itself write is how the db drifts from disk.
     */
    void registerValidPath(const ValidPathInfo & info);

    void verifyPath(
        const StorePath & path,
        const fun<bool(const StorePath &)> & existsInStoreDir,
        StorePathSet & done,
        StorePathSet & validPaths,
        bool & errors);

private:

    /**
     * Retrieve the current version of the database schema.
     * If the database does not exist yet, the version returned will be 0.
     */
    int getSchema();

    void openDB(State & state, bool create);

    void requireWritableStore();

    uint64_t queryValidPathId(State & state, const StorePath & path);

    uint64_t addValidPath(State & state, const ValidPathInfo & info);

    void invalidatePath(State & state, const StorePath & path);

    /**
     * Delete a path from the Nix store.
     */
    void invalidatePathChecked(const StorePath & path);

    std::shared_ptr<const ValidPathInfo> queryPathInfoUnchecked(const StorePath & path);

    std::shared_ptr<const ValidPathInfo> queryPathInfoInternal(State & state, const StorePath & path);

    void queryReferrers(const StorePath & path, StorePathSet & referrers);

    /**
     * Called by `collectGarbage` to recursively delete a path.
     */
    void deleteStorePath(const std::filesystem::path & path, uint64_t & bytesFreed);

    void updatePathInfo(State & state, const ValidPathInfo & info);

    void findRoots(const std::filesystem::path & path, std::filesystem::file_type type, Roots & roots);

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
        OptimiseCtx & ctx,
        const std::filesystem::path & path,
        bool * parentToggled = nullptr,
        ThreadPool * pool = nullptr);

    // Internal versions that are not wrapped in retry_sqlite.
    bool isValidPath_(State & state, const StorePath & path);
    void queryReferrers(State & state, const StorePath & path, StorePathSet & referrers);

};

} // namespace nix
