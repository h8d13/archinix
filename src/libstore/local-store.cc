#include "nix/store/local-store.hh"
#include "nix/store/globals.hh"
#include "nix/util/archive.hh"
#include "nix/store/pathlocks.hh"
#include "nix/util/finally.hh"
#include "nix/util/signals.hh"
#include "nix/store/posix-fs-canonicalise.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/file-system-at.hh"
#include "nix/util/fs-sink.hh"
#include "nix/util/progress.hh"
#include "nix/store/store-open.hh"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <random>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <sys/types.h>
#if NIX_SUPPORT_ACL
#  include <sys/xattr.h>
#endif
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>
#include <utime.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>

#  include <grp.h>



#include <sqlite3.h>


#include "nix/util/strings.hh"

#include "store-config-private.hh"

namespace nix {

LocalStoreConfig::LocalStoreConfig(const std::filesystem::path & rootDir)
    : StoreDirConfig{logicalStoreDir()}
    , rootDir(canonPath(rootDir))
    , stateDir(this->rootDir / "nix" / "var" / "nix")
    , realStoreDir(this->rootDir / "nix" / "store")
{
}

/* The logical store dir is baked in: the initramfs and GRUB entries
   spell out /nix/store, so only the *physical* half (realStoreDir) can
   move with the root. Kept as a function-local static so it outlives
   every config that references it. */
const std::string & LocalStoreConfig::logicalStoreDir()
{
    static const std::string dir = [] {
        std::filesystem::path p{NIX_STORE_DIR};
        if (!p.is_absolute())
            throw UsageError("store directory path %s is not an absolute path", PathFmt(p));
        return canonPath(std::move(p)).string();
    }();
    return dir;
}

ref<LocalStore> LocalStore::Config::openStore() const
{
    return make_ref<LocalStore>(ref{shared_from_this()});
}

/* A default POSIX ACL on the store dir would make every node the
   restore creates inherit ACL xattrs, which only the full
   canonicalisePathMetaData walk strips. One llistxattr on the store
   dir per import decides whether the canonical restore (which skips
   that walk) is safe; a store on an ACL-defaulted directory is a
   pathological layout, not the common case paying for it. */
static bool dirGrantsDefaultAcl(const std::filesystem::path & dir)
{
#if NIX_SUPPORT_ACL
    ssize_t eaSize = llistxattr(dir.c_str(), nullptr, 0);
    if (eaSize <= 0)
        return false;
    std::vector<char> eaBuf(eaSize);
    if ((eaSize = llistxattr(dir.c_str(), eaBuf.data(), eaBuf.size())) < 0)
        return false;
    for (auto & eaName :
         tokenizeString<Strings>(std::string(eaBuf.data(), eaSize), std::string("\000", 1)))
        if (eaName == "system.posix_acl_default")
            return true;
#endif
    return false;
}


struct LocalStore::State::Stmts
{
    /* Some precompiled SQLite statements. */
    SQLiteStmt RegisterValidPath;
    SQLiteStmt UpdatePathInfo;
    SQLiteStmt AddReference;
    SQLiteStmt QueryPathInfo;
    SQLiteStmt QueryReferences;
    SQLiteStmt QueryReferrers;
    SQLiteStmt InvalidatePath;
    SQLiteStmt QueryPathsByHashPrefix;
    SQLiteStmt QueryValidPaths;
};

/* The schema file identifies the store. Writing it in place is not
   safe: writeFull() polls checkInterrupt(), so a Ctrl-C (or a crash,
   or power loss) between create and write leaves a zero-byte file,
   which getSchema() then rejects as corrupt and which no tool can
   recover from. Stage it and rename: rename is atomic, so an
   interrupt leaves either no schema at all (the store reads as new
   and is recreated) or the complete one. */
static void writeSchemaFile(const std::filesystem::path & schemaPath, int version)
{
    auto tmp = schemaPath.parent_path() / (schemaPath.filename().native() + ".tmp");
    writeFile(tmp, fmt("%1%", version), 0666, FsSync::Yes);
    std::filesystem::rename(tmp, schemaPath);
}

LocalStore::LocalStore(ref<const Config> config)
    : StoreDirConfig{*config}
    , config{config}
    , _state(make_ref<Sync<State>>())
    , dbDir(config->stateDir / "db")
    , linksDir(config->realStoreDir / ".links")
    , reservedPath(dbDir / "reserved")
    , schemaPath(dbDir / "schema")
{
    assertLibStoreInitialized();

    auto state(_state->lock());
    state->stmts = std::make_unique<State::Stmts>();

    /* Create missing state directories if they don't already exist. */
    createDirs(config->realStoreDir);
    if (!config->readOnly) {
        requireWritableStore();
        /* Close the store dir before anything is created inside it.
           Imported trees carry secrets (shadow, ssh host keys) at
           canonical 0444, and this inode is the one path-credential
           gate (arch/import-dir.cc says why, and re-applies it on every
           import to heal older stores). createDirs goes through
           std::filesystem::create_directories, which has no mode
           argument and lands 0777&~umask, so the farm, the db and the
           gcroots used to be created during a world-traversable
           window. */
        if (::chmod(config->realStoreDir.c_str(), 0700) == -1)
            throw SysError("securing store directory %s", PathFmt(config->realStoreDir));
    }
    createDirs(linksDir);
    auto profilesDir = config->stateDir / "profiles";
    createDirs(profilesDir);
    createDirs(dbDir);
    auto gcRootsDir = config->stateDir / "gcroots";
    const auto & localSettings = config->getLocalSettings();
    createDirs(gcRootsDir);

    for (auto & perUserDir : {profilesDir / "per-user", gcRootsDir / "per-user"}) {
        createDirs(perUserDir);
        if (!config->readOnly) {
            // Skip chmod call if the directory already has the correct permissions (0755).
            // This is to avoid failing when the executing user lacks permissions to change the directory's permissions
            // even if it would be no-op.
            chmodIfNeeded(perUserDir, 0755, S_IRWXU | S_IRWXG | S_IRWXO);
        }
    }

    /* Ensure that the store and its parents are not symlinks. */
    if (!localSettings.allowSymlinkedStore) {
        std::filesystem::path path = config->realStoreDir;
        std::filesystem::path root = path.root_path();
        while (path != root) {
            if (std::filesystem::is_symlink(path))
                throw Error(
                    "the path %1% is a symlink; "
                    "this is not allowed for the Nix store and its parent directories",
                    PathFmt(path));
            path = path.parent_path();
        }
    }

    /* We can't open a SQLite database if the disk is full.  Since
       this prevents the garbage collector from running when it's most
       needed, we reserve some dummy space that we can free just
       before doing a garbage collection. */
    try {
        auto st = maybeStat(reservedPath);
        if (!st || st->st_size != localSettings.reservedSize) {
            AutoCloseFD fd = (open(
                reservedPath.string().c_str(),
                O_WRONLY | O_CREAT
                    | O_CLOEXEC
                ,
                0600));
            int res = -1;
#if HAVE_POSIX_FALLOCATE
            res = posix_fallocate(fd.get(), 0, localSettings.reservedSize);
#endif
            if (res != 0) {
                writeFull(fd.get(), std::string(localSettings.reservedSize, 'X'));
                [[gnu::unused]] auto res2 =

                    ftruncate(fd.get(), localSettings.reservedSize)
                    ;
            }
        }
    } catch (SystemError & e) { /* don't care about errors */
    }

    /* One schema, created here or not ours. There is no migration
       ladder: a store is minted by this tool and rebuilt when the
       schema changes, so any other version means the path points at
       somebody else's database. Refuse rather than rewrite it. */
    int curSchema = getSchema();

    if (curSchema == 0) {
        if (config->readOnly)
            throw Error("database does not exist, and cannot be created in read-only mode");
        openDB(*state, true);
        writeSchemaFile(schemaPath, nixSchemaVersion);
    }

    else if (curSchema != nixSchemaVersion)
        throw Error(
            "store database at '%s' has schema version %d, but this store requires %d; "
            "it was not created by this tool",
            PathFmt(schemaPath),
            curSchema,
            nixSchemaVersion);

    else
        openDB(*state, false);

    /* Prepare SQL statements. */
    state->stmts->RegisterValidPath.create(
        state->db,
        "insert into ValidPaths (path, hash, registrationTime, narSize, ca) values (?, ?, ?, ?, ?);");
    state->stmts->UpdatePathInfo.create(
        state->db, "update ValidPaths set narSize = ?, hash = ?, ca = ? where path = ?;");
    state->stmts->AddReference.create(state->db, "insert or replace into Refs (referrer, reference) values (?, ?);");
    state->stmts->QueryPathInfo.create(
        state->db,
        "select id, hash, registrationTime, narSize, ca from ValidPaths where path = ?;");
    state->stmts->QueryReferences.create(
        state->db, "select path from Refs join ValidPaths on reference = id where referrer = ?;");
    state->stmts->QueryReferrers.create(
        state->db,
        "select path from Refs join ValidPaths on referrer = id where reference = (select id from ValidPaths where path = ?);");
    state->stmts->InvalidatePath.create(state->db, "delete from ValidPaths where path = ?;");
    // Use "path >= ?" with limit 1 rather than "path like '?%'" to
    // ensure efficient lookup.
    state->stmts->QueryPathsByHashPrefix.create(state->db, "select path from ValidPaths where path >= ? order by path;");
    state->stmts->QueryValidPaths.create(state->db, "select path from ValidPaths");
}

AutoCloseFD LocalStore::openGCLock()
{
    auto fnGCLock = config->stateDir / "gc.lock";
    return openLockFile(fnGCLock, /*create=*/true);
}

void LocalStore::deleteStorePath(const std::filesystem::path & path, uint64_t & bytesFreed)
{
    try {
        deletePath(path, bytesFreed);
    } catch (SystemError & e) {
        if (config->ignoreGcDeleteFailure) {
            logWarning({.msg = HintFmt("ignoring failure to remove store path %1%: %2%", PathFmt(path), e.info().msg)});
        } else {
            e.addTrace("While deleting store path %1%", PathFmt(path));
            throw;
        }
    }
}

LocalStore::~LocalStore() {}

int LocalStore::getSchema()
{
    int curSchema = 0;
    if (pathExists(schemaPath)) {
        auto s = readFile(schemaPath);
        auto n = string2Int<int>(s);
        if (!n)
            throw Error("%1% is corrupt", PathFmt(schemaPath));
        curSchema = *n;
    }
    return curSchema;
}

void LocalStore::openDB(State & state, bool create)
{
    if (create && config->readOnly) {
        throw Error("cannot create database while in read-only mode");
    }

    if (access(dbDir.string().c_str(), R_OK | (config->readOnly ? 0 : W_OK)))
        throw SysError("Nix database directory %1% is not writable", PathFmt(dbDir));

    /* Open the Nix database. */
    auto & db(state.db);
    auto openMode = config->readOnly ? SQLiteOpenMode::Immutable
                    : create         ? SQLiteOpenMode::Normal
                                     : SQLiteOpenMode::NoCreate;
    state.db = SQLite(dbDir / "db.sqlite", {.mode = openMode, .useWAL = config->getLocalSettings().useSQLiteWAL});


    /* !!! check whether sqlite has been built with foreign key
       support */

    /* Whether SQLite should fsync().  "Normal" synchronous mode
       should be safe enough.  If the user asks for it, don't sync at
       all.  This can cause database corruption if the system
       crashes. */
    std::string syncMode = config->getLocalSettings().fsyncMetadata ? "normal" : "off";
    db.exec("pragma synchronous = " + syncMode);

    /* Set the SQLite journal mode.  WAL mode is fastest, so it's the
       default. */
    std::string mode = config->getLocalSettings().useSQLiteWAL ? "wal" : "truncate";
    std::string prevMode;
    {
        SQLiteStmt stmt;
        stmt.create(db, "pragma main.journal_mode;");
        if (sqlite3_step(stmt) != SQLITE_ROW)
            SQLiteError::throw_(db, "querying journal mode");
        prevMode = std::string((const char *) sqlite3_column_text(stmt, 0));
    }
    if (prevMode != mode
        && sqlite3_exec(db, ("pragma main.journal_mode = " + mode + ";").c_str(), 0, 0, 0) != SQLITE_OK)
        SQLiteError::throw_(db, "setting journal mode");

    if (mode == "wal") {
        /* persist the WAL files when the db connection is closed. This allows
           for read-only connections without write permissions on the
           containing directory to succeed on a closed db. Setting the
           journal_size_limit to 2^40 bytes results in the WAL files getting
           truncated to 0 on exit and limits the on disk size of the WAL files
           to 2^40 bytes following a checkpoint */
        if (sqlite3_exec(db, "pragma main.journal_size_limit = 1099511627776;", 0, 0, 0) == SQLITE_OK) {
            int enable = 1;
            sqlite3_file_control(db, NULL, SQLITE_FCNTL_PERSIST_WAL, &enable);
        }
    }

    /* Increase the auto-checkpoint interval to 40000 pages.  This
       seems enough to ensure that instantiating the NixOS system
       derivation is done in a single fsync(). */
    if (mode == "wal" && sqlite3_exec(db, "pragma wal_autocheckpoint = 40000;", 0, 0, 0) != SQLITE_OK)
        SQLiteError::throw_(db, "setting autocheckpoint interval");

    /* Initialise the database schema, if necessary. */
    if (create) {
        static const char schema[] =
#include "schema.sql.gen.hh"
            ;
        db.exec(schema);
    }
}

/* The store dir must be writable to import into it. Remounting it is
   deliberately not our job: arch/ owns the mount table (the initramfs
   overlay-mounts the generation), and a remount from in here would
   escape into the host namespace. Refuse and say so. */
void LocalStore::requireWritableStore()
{
    struct statvfs st;
    auto dir = config->realStoreDir;
    if (statvfs(dir.c_str(), &st) != 0)
        throw SysError("getting mount info for %s", PathFmt(dir));
    if (st.f_flag & ST_RDONLY)
        throw Error(
            "store directory %s is on a read-only mount; "
            "mount it writable before importing",
            PathFmt(dir));
}




uint64_t LocalStore::addValidPath(State & state, const ValidPathInfo & info)
{
    if (info.ca.has_value() && !info.isContentAddressed(*this))
        throw Error(
            "cannot add path '%s' to the Nix store because it claims to be content-addressed but isn't",
            printStorePath(info.path));

    state.stmts->RegisterValidPath.use()
        .apply(printStorePath(info.path))
        .apply(info.narHash.to_string(HashFormat::Base16, true))
        .apply(info.registrationTime == 0 ? time(nullptr) : info.registrationTime)
        .apply(info.narSize, info.narSize != 0)
        .apply(renderContentAddress(info.ca), (bool) info.ca)
        .exec();
    uint64_t id = state.db.getLastInsertedRowId();

    return id;
}

std::shared_ptr<const ValidPathInfo> LocalStore::queryPathInfoUnchecked(const StorePath & path)
{
    return retrySQLite<std::shared_ptr<const ValidPathInfo>>([&]() {
        return queryPathInfoInternal(*_state->lock(), path);
    });
}

std::shared_ptr<const ValidPathInfo> LocalStore::queryPathInfoInternal(State & state, const StorePath & path)
{
    /* Get the path info. */
    auto useQueryPathInfo(state.stmts->QueryPathInfo.use().apply(printStorePath(path)));

    if (!useQueryPathInfo.next())
        return std::shared_ptr<ValidPathInfo>();

    auto id = useQueryPathInfo.getInt(0);

    Hash narHash;
    try {
        narHash = Hash::parseAnyPrefixed(useQueryPathInfo.getStr(1));
    } catch (BadHash & e) {
        throw Error("invalid-path entry for '%s': %s", printStorePath(path), e.what());
    }

    auto info = std::make_shared<ValidPathInfo>(path, UnkeyedValidPathInfo(*this, narHash));

    info->registrationTime = useQueryPathInfo.getInt(2);

    /* Note that narSize = NULL yields 0. */
    info->narSize = useQueryPathInfo.getInt(3);

    auto s = (const char *) sqlite3_column_text(state.stmts->QueryPathInfo, 4);
    if (s)
        info->ca = ContentAddress::parseOpt(s);

    /* Get the references. */
    auto useQueryReferences(state.stmts->QueryReferences.use().apply(id));

    while (useQueryReferences.next())
        info->references.insert(parseStorePath(useQueryReferences.getStr(0)));

    return info;
}

/* Update path info in the database. */
void LocalStore::updatePathInfo(State & state, const ValidPathInfo & info)
{
    state.stmts->UpdatePathInfo.use()
        .apply(info.narSize, info.narSize != 0)
        .apply(info.narHash.to_string(HashFormat::Base16, true))
        .apply(renderContentAddress(info.ca), (bool) info.ca)
        .apply(printStorePath(info.path))
        .exec();
}

uint64_t LocalStore::queryValidPathId(State & state, const StorePath & path)
{
    auto use(state.stmts->QueryPathInfo.use().apply(printStorePath(path)));
    if (!use.next())
        throw InvalidPath("path '%s' is not valid", printStorePath(path));
    return use.getInt(0);
}

bool LocalStore::isValidPath_(State & state, const StorePath & path)
{
    return state.stmts->QueryPathInfo.use().apply(printStorePath(path)).next();
}

bool LocalStore::isValidPath(const StorePath & path)
{
    return retrySQLite<bool>([&]() { return isValidPath_(*_state->lock(), path); });
}


StorePathSet LocalStore::queryPathsByHashPrefix(const std::string & hashPrefix)
{
    if (hashPrefix.empty() || hashPrefix.size() > StorePath::HashLen)
        throw Error("invalid hash prefix '%s'", hashPrefix);

    std::string prefix = storeDir + "/" + hashPrefix;

    return retrySQLite<StorePathSet>([&]() {
        auto state(_state->lock());

        /* `path >= prefix` ordered by path puts every match in one
           contiguous run, so walk until a row falls outside it. A
           partial id (what nixgen-listid prints) is a prefix like any
           other; the caller decides what more than one match means. */
        auto use(state->stmts->QueryPathsByHashPrefix.use().apply(prefix));

        StorePathSet res;
        while (use.next()) {
            auto path = use.getStr(0);
            if (!path.starts_with(prefix))
                break;
            res.insert(parseStorePath(path));
        }
        return res;
    });
}

StorePathSet LocalStore::queryAllValidPaths()
{
    return retrySQLite<StorePathSet>([&]() {
        auto state(_state->lock());
        auto use(state->stmts->QueryValidPaths.use());
        StorePathSet res;
        while (use.next())
            res.insert(parseStorePath(use.getStr(0)));
        return res;
    });
}

void LocalStore::queryReferrers(State & state, const StorePath & path, StorePathSet & referrers)
{
    auto useQueryReferrers(state.stmts->QueryReferrers.use().apply(printStorePath(path)));

    while (useQueryReferrers.next())
        referrers.insert(parseStorePath(useQueryReferrers.getStr(0)));
}

void LocalStore::queryReferrers(const StorePath & path, StorePathSet & referrers)
{
    return retrySQLite<void>([&]() { queryReferrers(*_state->lock(), path, referrers); });
}

/* Upstream took a `ValidPathInfos` map here so the daemon could
   register a whole closure in one transaction. Every caller left
   registers exactly one path, so the map, its two loops and the
   typedef are gone. */
void LocalStore::registerValidPath(const ValidPathInfo & info)
{
    /* SQLite will fsync by default, but the new valid paths may not
       be fsync-ed.  So some may want to fsync them before registering
       the validity, at the expense of some speed of the path
       registering operation. */
    if (config->getLocalSettings().syncBeforeRegistering)
        sync();

    return retrySQLite<void>([&]() {
        auto state(_state->lock());

        SQLiteTxn txn(state->db);

        if (isValidPath_(*state, info.path))
            updatePathInfo(*state, info);
        else
            addValidPath(*state, info);

        /* Refs rows, for a reference set that the import door keeps
           empty: this is what would make a bypass visible to the
           `on delete restrict` FK. No cycle check follows it, since a
           cycle needs edges this store does not have. */
        if (!info.references.empty()) {
            auto referrer = queryValidPathId(*state, info.path);
            for (auto & j : info.references)
                state->stmts->AddReference.use().apply(referrer).apply(queryValidPathId(*state, j)).exec();
        }

        txn.commit();
    });
}

/* Invalidate a path.  The caller is responsible for checking that
   there are no referrers. */
void LocalStore::invalidatePath(State & state, const StorePath & path)
{
    debug("invalidating path '%s'", printStorePath(path));

    state.stmts->InvalidatePath.use().apply(printStorePath(path)).exec();

    /* Note that the foreign key constraints on the Refs table take
       care of deleting the references entries for `path'. */

}

void LocalStore::addToStore(const ValidPathInfo & info, Source & source)
{
    {

        if (!isValidPath(info.path)) {

            PathLocks outputLock;

            auto realPath = toRealPath(info.path);

            outputLock.lockPaths({realPath});

            /* The path may have been created by another process in the meantime, so check again. */
            if (!isValidPath(info.path)) {

                deletePath(realPath);

                /* While restoring the path from the NAR, compute the hash
                   of the NAR. */
                HashSink hashSink;

                TeeSource wrapperSource{source, hashSink};

                bool canonicalRestore = !dirGrantsDefaultAcl(config->realStoreDir);
                restorePath(
                    realPath, wrapperSource, config->getLocalSettings().fsyncStorePaths, canonicalRestore);

                auto hashResult = hashSink.finish();

                if (hashResult.hash != info.narHash)
                    throw Error(
                        "hash mismatch importing path '%s';\n  specified: %s\n  got:       %s",
                        printStorePath(info.path),
                        info.narHash.to_string(HashFormat::SRI, true),
                        hashResult.hash.to_string(HashFormat::SRI, true));

                if (hashResult.numBytesDigested != info.narSize)
                    throw Error(
                        "size mismatch importing path '%s';\n  specified: %s\n  got:       %s",
                        printStorePath(info.path),
                        info.narSize,
                        hashResult.numBytesDigested);

                /* No re-hash against info.ca here: the only caller
                   (import-path) builds its ValidPathInfo from the NAR
                   it just read and never carries a ca, and it has
                   already recomputed the store path from that hash
                   before calling. addValidPath still checks ca for the
                   paths that do carry one (addToStoreFromDump). */

                /* the canonical restore stamped every node except the
                   root (see restorePath); finish it in place */
                if (canonicalRestore)
                    canonicaliseTimestampAndPermissions(realPath);
                else
                    canonicalisePathMetaData(
                        realPath, {NIX_WHEN_SUPPORT_ACLS(config->getLocalSettings().ignoredAcls)});

                optimisePath(realPath); // FIXME: combine with hashPath()

                if (config->getLocalSettings().fsyncStorePaths) {
                    recursiveSync(realPath);
                    syncParent(realPath);
                }

                registerValidPath(info);
            }

            outputLock.setDeletion(true);
        }
    }
}

/* True when farming hash work out to a thread can actually overlap
   with the producer; on a single hardware thread the queueing is pure
   overhead (chunk copies + context-switch churn). */
static bool hashingThreadPaysOff()
{
    return std::thread::hardware_concurrency() > 1;
}

/* Runs a HashSink on its own thread. An import hashes the stream
   twice (whole-NAR digest for the store path, per-file digests for
   dedup); this takes one of them off the restore's critical path.
   Bounded queue so a fast producer cannot balloon memory. Hashes
   inline on single-core machines.

   Buffered, for the reason sourceToSink is: this sink is fed by the
   TeeSource the *restore* reads through, and parseDump reads the NAR
   the way it is framed -- an 8 byte length word, a tag, the contents,
   a padding run, per node. Unbuffered, each of those tiny reads
   arrived here as its own queue push: an allocation, a mutex and a
   notify apiece. Measured on a 3.3 GiB driver generation: 1,740,668
   pushes averaging 2005 bytes, against 193,620 at 18 KB on the
   per-file hasher next to it. The dump direction got this fix when
   sourceToSink learned to buffer; the restore direction reads through
   the same shape and never did. */
class AsyncHashSink : public BufferedSink
{
    HashSink inner;
    bool threaded;
    std::thread worker;
    std::mutex mtx;
    std::condition_variable cvPush, cvPop;
    std::deque<std::string> chunks;
    /* Spent buffers, handed back by the worker for the producer to
       refill: at 32-64 KiB a chunk, a multi-GiB import is otherwise
       hundreds of thousands of allocate/free pairs. sourceToSink was
       rebuilt around exactly this and the reasoning transfers whole. A
       LIFO because any spare buffer will do, and bounded by maxQueued
       + 1 because that is how many exist. */
    std::vector<std::string> free_;
    std::string producing;
    bool closed = false;
    std::exception_ptr failure;
    static constexpr size_t maxQueued = 64;

public:
    AsyncHashSink()
        : threaded(hashingThreadPaysOff())
    {
        if (!threaded)
            return;
        worker = std::thread([this] {
            std::unique_lock lk(mtx);
            while (true) {
                cvPush.wait(lk, [&] { return closed || !chunks.empty(); });
                if (chunks.empty())
                    return;
                auto chunk = std::move(chunks.front());
                chunks.pop_front();
                /* Only a pop off a *full* queue can release the
                   producer, and there is exactly one producer (the
                   restore thread). Notifying on every pop is a futex
                   wake per chunk with nobody waiting on it. */
                bool wake = chunks.size() == maxQueued - 1;
                lk.unlock();
                if (wake)
                    cvPop.notify_one();
                /* on failure keep draining so the producer never
                   blocks on a full queue; rethrow at finish() */
                if (!failure) {
                    try {
                        inner(chunk);
                    } catch (...) {
                        failure = std::current_exception();
                    }
                }
                lk.lock();
                chunk.clear();
                free_.push_back(std::move(chunk));
            }
        });
    }

    void writeUnbuffered(std::string_view data) override
    {
        if (!threaded) {
            inner(data);
            return;
        }
        /* filled outside the lock; the producer owns this buffer */
        producing.assign(data);
        std::unique_lock lk(mtx);
        cvPop.wait(lk, [&] { return chunks.size() < maxQueued; });
        chunks.push_back(std::move(producing));
        if (!free_.empty()) {
            producing = std::move(free_.back());
            free_.pop_back();
        } else
            producing.clear();
        /* The worker only ever waits after observing an empty queue
           under this mutex, so a push that leaves the queue non-empty
           by more than itself cannot have raced one to sleep. */
        bool wake = chunks.size() == 1;
        lk.unlock();
        if (wake)
            cvPush.notify_one();
    }

    void close()
    {
        {
            std::lock_guard<std::mutex> lk(mtx);
            closed = true;
        }
        cvPush.notify_one();
        if (worker.joinable())
            worker.join();
    }

    HashResult finish()
    {
        /* the tail of the stream is still in the buffer; the worker
           has to digest it before the queue is closed */
        flush();
        close();
        if (failure)
            std::rethrow_exception(failure);
        return inner.finish();
    }

    ~AsyncHashSink()
    {
        try {
            /* ~BufferedSink asserts on an unflushed buffer, so this
               has to run even on the error path */
            flush();
            close();
        } catch (...) {
            ignoreExceptionInDestructor();
        }
    }
};

/* Where a restored file lives, from the tree root and the map key the
   hasher already built. */
static std::filesystem::path farmTarget(const std::filesystem::path & dedupRoot, const std::string & key)
{
    return std::filesystem::path{dedupRoot.native() + (key == "/" ? std::string() : key)};
}

/* One dedup swap: hard-link the farm entry to a temp name, rename it
   over the just-restored file. True when the file was replaced.

   The temp link goes in the store root, never beside the file:
   "<file>.dedup~" is a name the imported tree may itself contain, and
   then this link races the restore thread's O_CREAT|O_EXCL for it and
   whichever loses aborts the whole import. It would also leave junk
   *inside* a store path if we died between link and rename, which
   breaks that path's hash. optimisePath_ places its own temp link in
   the store root for exactly these reasons.

   Callable from any thread: the temp name carries an atomic counter, and
   two swaps never name the same file. */
static bool swapForFarmLink(
    const std::filesystem::path & link,
    const std::filesystem::path & file,
    const std::filesystem::path & storeRoot)
{
    static std::atomic<uint32_t> tmpCounter(std::random_device{}());
    /* getpid() is a real syscall since glibc 2.25 dropped its cache,
       and it cannot change under us: resolve once. */
    static const std::string tmpPart = "/.tmp-dedup-" + std::to_string(getpid()) + "-";
    std::filesystem::path tmp{
        storeRoot.native() + tmpPart + std::to_string(tmpCounter.fetch_add(1, std::memory_order_relaxed))};
    if (::link(link.c_str(), tmp.c_str()) == -1)
        return false;
    if (::rename(tmp.c_str(), file.c_str()) == -1) {
        /* A leftover temp link is inert in the store root, but it is
           still a leak and it means the rename failed for a reason worth
           hearing about: the mode/mtime ordering trap this code has hit
           before shows up here first. */
        if (::unlink(tmp.c_str()) == -1)
            warn("dedup: cannot unlink %s: %s", PathFmt(tmp), strerror(errno));
        return false;
    }
    return true;
}

/* The dedup swaps of an import, off the hashing thread.

   Hashing is one thread and stays one thread: a NAR is one stream and
   the digest that names the store path is a serial SHA-256 over it, so
   there is nothing there to fan out (measured, see the DISCARDED entry
   in bench/BASELINE). The swap is different work with a different
   bound. It is `link` plus `rename`, two journaled metadata operations
   whose cost is a round trip to the device, not CPU, and on a re-commit
   nearly every file takes it: 30,209 of 38,431 on the generation tree.
   Serialised behind the digests, that latency was the hashing thread's
   whole day; issued from a few threads, the device sees them
   concurrently.

   One queue, N consumers, because unlike the per-file event stream
   these jobs are independent and unordered. Bounded, so a fast hasher
   cannot balloon it. Opportunistic like the swap itself: a job that
   fails leaves the copy for optimisePath() to farm, and nothing here
   ever throws into the import.

   The pool must be drained before the deferred directory
   canonicalisation runs, since a swap bumps the containing directory's
   mtime; `AsyncFileHasher::finish` joins the hashing thread first and
   this second, which is exactly that order. */
class DedupSwapPool
{
    struct Job
    {
        std::filesystem::path link;
        std::string key;
        uint64_t size;
    };

    const std::filesystem::path & dedupRoot;
    const std::filesystem::path & storeRoot;
    std::vector<std::thread> workers;
    std::mutex mtx;
    std::condition_variable cvPush, cvPop;
    std::deque<Job> jobs;
    bool closed = false;
    static constexpr size_t maxQueued = 64;
    /* summed into the caller's totals by finish() */
    std::atomic<uint64_t> swappedFiles{0};
    std::atomic<uint64_t> swappedBytes{0};

    void run()
    {
        std::unique_lock lk(mtx);
        while (true) {
            cvPush.wait(lk, [&] { return closed || !jobs.empty(); });
            if (jobs.empty())
                return;
            auto job = std::move(jobs.front());
            jobs.pop_front();
            /* only a pop off a full queue can release the producer */
            bool wake = jobs.size() == maxQueued - 1;
            lk.unlock();
            if (wake)
                cvPop.notify_one();
            try {
                if (swapForFarmLink(job.link, farmTarget(dedupRoot, job.key), storeRoot)) {
                    swappedFiles.fetch_add(1, std::memory_order_relaxed);
                    swappedBytes.fetch_add(job.size, std::memory_order_relaxed);
                }
            } catch (...) {
                /* opportunistic */
            }
            lk.lock();
        }
    }

public:
    DedupSwapPool(size_t threads, const std::filesystem::path & dedupRoot, const std::filesystem::path & storeRoot)
        : dedupRoot(dedupRoot)
        , storeRoot(storeRoot)
    {
        for (size_t i = 0; i < threads; i++)
            workers.emplace_back([this] { run(); });
    }

    void post(Job job)
    {
        std::unique_lock lk(mtx);
        cvPop.wait(lk, [&] { return jobs.size() < maxQueued; });
        jobs.push_back(std::move(job));
        /* a consumer only ever waits after observing an empty queue
           under this mutex, so waking one per non-empty edge cannot
           race one to sleep; with N consumers the edge has to wake all
           of them, or a burst leaves workers asleep beside a full
           queue */
        bool wake = jobs.size() <= workers.size();
        lk.unlock();
        if (wake)
            cvPush.notify_all();
    }

    /* Drains, joins, and adds what it replaced to the caller's totals.
       Idempotent: finish() and ~DedupSwapPool both call it, and the
       counters are moved out rather than copied so a second call adds
       nothing twice. */
    void finish(uint64_t & files, uint64_t & bytes)
    {
        {
            std::lock_guard<std::mutex> lk(mtx);
            closed = true;
        }
        cvPush.notify_all();
        for (auto & w : workers)
            if (w.joinable())
                w.join();
        files += swappedFiles.exchange(0);
        bytes += swappedBytes.exchange(0);
    }

    ~DedupSwapPool()
    {
        try {
            uint64_t files = 0, bytes = 0;
            finish(files, bytes);
        } catch (...) {
            ignoreExceptionInDestructor();
        }
    }
};

/* Hashes each regular file's single-file NAR serialisation (same
   framing SourceAccessor::dumpPath emits, so the digest equals what
   hashPath() would recompute from disk) on a worker thread, fed a
   strictly ordered event stream by the restore below; inline on
   single-core machines. Contents are hashed once, at restore time,
   off the critical path, and optimisePath() never has to read them
   back. */
class AsyncFileHasher
{
    struct Ev
    {
        enum
        {
            Begin, /* data = map key */
            Exec,
            Size, /* size = contents length */
            Data, /* data = contents chunk */
            End,
            Symlink, /* data = map key, target = link target */
        } tag;

        std::string data;
        std::string target;
        uint64_t size = 0;
    };

    LocalStore::ImportFileHashes & out;

    /* when set, end() swaps a just-restored file for a hard link into
       the farm if its content is already there (streaming dedup) */
    const std::filesystem::path * dedupRoot;
    const std::filesystem::path * linksDir;
    /* the store root, parent of the farm: where tryDedup's temp link
       goes, so it can never collide with a name inside the tree being
       restored */
    std::filesystem::path storeRootPath;
    const std::filesystem::path * storeRoot = nullptr;
    /* declared after storeRootPath so it is destroyed before the path
       it holds a reference to; null means swap inline, as before */
    std::unique_ptr<DedupSwapPool> swaps;

    bool threaded;
    std::thread worker;
    std::mutex mtx;
    std::condition_variable cvPush, cvPop;
    std::deque<Ev> events;
    /* recycled data buffers; see AsyncHashSink */
    std::vector<std::string> free_;
    std::string producing;
    bool closed = false;
    std::exception_ptr failure;
    static constexpr size_t maxQueued = 64;

    /* per-file state machine; worker-owned when threaded, caller-owned
       otherwise (events per file are strictly ordered either way, so
       the digests are identical) */
    std::string key;
    std::unique_ptr<HashSink> hash;
    uint64_t size = 0;
    /* scratch for tryDedup's farm probe, reused across files */
    std::string linkBuf;
    /* every content byte the import has hashed, across all files:
       what keeps the counter moving through a single large blob */
    uint64_t bytesDone = 0;

    void begin(std::string k)
    {
        key = std::move(k);
        hash = std::make_unique<HashSink>();
        *hash << narVersionMagic1 << "(" << "type" << "regular";
    }

    void exec()
    {
        *hash << "executable" << "";
    }

    void setSize(uint64_t s)
    {
        size = s;
        *hash << "contents" << s;
    }

    void data(std::string_view d)
    {
        (*hash)(d);
        bytesDone += d.size();
        /* throttled to one repaint per 80 ms inside progressTick, and
           a cached isTTY() check off a terminal, so this is cheap
           enough to call per chunk */
        progressTick("importing", out.files.size(), 0, bytesDone);
    }

    void end()
    {
        writePadding(size, *hash);
        *hash << ")";
        auto h = hash->finish().hash;
        tryDedup(h);
        out.files.insert_or_assign(std::move(key), h);
        hash.reset();
        /* the NAR is a stream with no length known up front, so this
           is a bare count; the caller's summary has the totals */
        progressTick("importing", out.files.size(), 0, bytesDone);
    }

    /* The restore has finished this file (the caller queues End only
       after the sink flushed and stamped it), so nothing touches it
       again: if the link farm holds its content (same NAR hash, so the
       execute bit matches too), swap the fresh copy for a hard link via
       link+rename and give the data pages back. The later
       canonicalise/optimise passes see a farm inode that is already
       canonical (0444/0555, mtime 1) and apply the same values.
       Opportunistic: any failure leaves the copy for optimisePath() to
       farm; never throws into the import. Empty files are skipped, same
       rule (and reason) as optimise.

       Split in two: the farm probe below stays on this thread (one
       lstat against a dentry the farm keeps warm, and it is what
       decides whether there is any work at all), while the swap itself
       goes to `swaps` when there is one. The swap is two journaled
       metadata operations, so on a re-commit where most files are
       duplicates it was tens of thousands of round trips serialised
       behind the digests. See bench/BASELINE, "the dedup swap, handed
       off". */
    void tryDedup(const Hash & h)
    {
        if (!dedupRoot || !linksDir || size == 0)
            return;
        try {
            /* built into a buffer this worker keeps: on a first commit
               every probe misses (only the optimise pass creates farm
               entries), so a path object per file was allocated,
               concatenated and discarded. The path is only materialised
               on a hit, below. */
            linkBuf.assign(linksDir->native());
            linkBuf += '/';
            linkBuf += h.to_string(HashFormat::Nix32, false);
            struct stat stLink;
            if (::lstat(linkBuf.c_str(), &stLink) == -1
                || uint64_t(stLink.st_size) != size)
                return;
            std::filesystem::path link{linkBuf};
            if (swaps) {
                swaps->post({std::move(link), key, size});
                return;
            }
            if (swapForFarmLink(link, farmTarget(*dedupRoot, key), *storeRoot)) {
                out.dedupedFiles++;
                out.dedupedBytes += size;
            }
        } catch (...) {
            /* opportunistic */
        }
    }

    /* A symlink's NAR is its target and nothing else, so its digest is
       a dozen bytes of framing rather than a content read. Capturing it
       here is what lets optimisePath look symlinks up in the map like
       regular files. Without it every symlink fell through to
       hashPath(), which opens the path O_NOFOLLOW, takes the ELOOP, then
       reopens the parent by full absolute path and readlinks: three
       syscalls and two allocations per symlink, and a store tree is
       roughly a quarter symlinks. Framing must match dumpPath's symlink
       branch exactly (archive.cc), or the farm gets entries filed under
       hashes that do not describe them. */
    void symlink(std::string k, const std::string & target)
    {
        HashSink h;
        h << narVersionMagic1 << "(" << "type" << "symlink" << "target" << target << ")";
        out.files.insert_or_assign(std::move(k), h.finish().hash);
        out.symlinks++;
    }

    void push(Ev ev)
    {
        std::unique_lock lk(mtx);
        cvPop.wait(lk, [&] { return events.size() < maxQueued; });
        events.push_back(std::move(ev));
        /* Refill the producer's scratch buffer from whatever the worker
           has handed back. Only Data events use it, but doing it on
           every push keeps one code path and costs a branch. */
        if (!free_.empty()) {
            producing = std::move(free_.back());
            free_.pop_back();
        } else
            producing.clear();
        /* See AsyncHashSink: wake only on the empty -> non-empty edge.
           This queue carries ~4 metadata events per file on top of the
           data chunks, so the elided wakes are most of them. */
        bool wake = events.size() == 1;
        lk.unlock();
        if (wake)
            cvPush.notify_one();
    }

    void run()
    {
        std::unique_lock lk(mtx);
        while (true) {
            cvPush.wait(lk, [&] { return closed || !events.empty(); });
            if (events.empty())
                return;
            auto ev = std::move(events.front());
            events.pop_front();
            bool wake = events.size() == maxQueued - 1;
            lk.unlock();
            if (wake)
                cvPop.notify_one();
            /* on failure keep draining so the producer never blocks
               on a full queue; rethrow at finish() */
            if (!failure) {
                try {
                    switch (ev.tag) {
                    case Ev::Begin:
                        begin(std::move(ev.data));
                        break;
                    case Ev::Exec:
                        exec();
                        break;
                    case Ev::Size:
                        setSize(ev.size);
                        break;
                    case Ev::Data:
                        data(ev.data);
                        break;
                    case Ev::End:
                        end();
                        break;
                    case Ev::Symlink:
                        symlink(std::move(ev.data), ev.target);
                        break;
                    }
                } catch (...) {
                    failure = std::current_exception();
                }
            }
            lk.lock();
            /* hand the data buffer back for the producer to refill;
               done on the drain-after-failure path too, or the pool
               bleeds out exactly when the queue is busiest */
            if (ev.tag == Ev::Data) {
                ev.data.clear();
                free_.push_back(std::move(ev.data));
            }
        }
    }

public:
    AsyncFileHasher(
        LocalStore::ImportFileHashes & out,
        const std::filesystem::path * dedupRoot,
        const std::filesystem::path * linksDir,
        size_t dedupThreads)
        : out(out)
        , dedupRoot(dedupRoot)
        , linksDir(linksDir)
        , threaded(hashingThreadPaysOff())
    {
        if (linksDir) {
            storeRootPath = linksDir->parent_path();
            storeRoot = &storeRootPath;
        }
        /* nothing to hand off without a farm to link from, and on a
           single-core machine the hashing is inline anyway: an extra
           thread there is the overhead without the overlap */
        if (threaded && dedupThreads && dedupRoot && linksDir)
            swaps = std::make_unique<DedupSwapPool>(dedupThreads, *dedupRoot, storeRootPath);
        if (threaded)
            worker = std::thread([this] { run(); });
    }

    /* Symlinks carry no content to stream, but they do carry a digest
       (over the target), and optimisePath() links them like any other
       node. Routed through the queue rather than recorded inline
       because `out` is worker-owned once the thread is running. */
    void fileSymlink(std::string k, const std::string & target)
    {
        if (threaded)
            push({Ev::Symlink, std::move(k), target});
        else
            symlink(std::move(k), target);
    }

    void fileBegin(std::string k)
    {
        if (threaded)
            push({Ev::Begin, std::move(k)});
        else
            begin(std::move(k));
    }

    void fileExec()
    {
        if (threaded)
            push({Ev::Exec});
        else
            exec();
    }

    void fileSize(uint64_t s)
    {
        if (threaded)
            push({Ev::Size, {}, {}, s});
        else
            setSize(s);
    }

    void fileData(std::string_view d)
    {
        if (threaded) {
            /* filled outside the lock into a buffer the producer owns;
               push() hands back a spent one to take its place */
            producing.assign(d);
            push({Ev::Data, std::move(producing)});
        } else
            data(d);
    }

    void fileEnd()
    {
        if (threaded)
            push({Ev::End});
        else
            end();
    }

    void close()
    {
        {
            std::lock_guard<std::mutex> lk(mtx);
            closed = true;
        }
        cvPush.notify_one();
        if (worker.joinable())
            worker.join();
        /* the hashing thread is the only producer of swap jobs, so it
           has to be joined before the pool is drained; the caller's
           deferred directory canonicalisation runs after both, which is
           what a swap's mtime bump needs */
        if (swaps)
            swaps->finish(out.dedupedFiles, out.dedupedBytes);
    }

    /* after this, `out` is complete and owned by the caller */
    void finish()
    {
        close();
        if (failure)
            std::rethrow_exception(failure);
    }

    ~AsyncFileHasher()
    {
        try {
            close();
        } catch (...) {
            ignoreExceptionInDestructor();
        }
    }
};

/* Forwards restore-sink calls to an inner sink, feeding regular-file
   events to the AsyncFileHasher on the side. */
struct FileHashingSink : FileSystemObjectSink
{
    FileSystemObjectSink & inner;
    CanonPath prefix;
    AsyncFileHasher & hasher;

    FileHashingSink(FileSystemObjectSink & inner, CanonPath prefix, AsyncFileHasher & hasher)
        : inner(inner)
        , prefix(std::move(prefix))
        , hasher(hasher)
    {
    }

    void createDirectory(const CanonPath & path) override
    {
        inner.createDirectory(path);
    }

    void createDirectory(const CanonPath & path, DirectoryCreatedCallback callback) override
    {
        inner.createDirectory(path, [&](FileSystemObjectSink & dirSink, const CanonPath & rel) {
            /* RestoreSink hands back a rerooted sink (rel = root); the
               default impl hands back itself (rel = path). Either way
               dirSink's root sits at prefix/path stripped of rel. */
            assert(rel.isRoot() || rel == path);
            FileHashingSink sub{dirSink, rel.isRoot() ? prefix / path : prefix, hasher};
            callback(sub, rel);
        });
    }

    void createSymlink(const CanonPath & path, const std::string & target) override
    {
        inner.createSymlink(path, target);
        hasher.fileSymlink((prefix / path).abs(), target);
    }

    void createRegularFile(const CanonPath & path, fun<void(CreateRegularFileSink &)> func) override
    {
        inner.createRegularFile(path, [&](CreateRegularFileSink & crf) {
            struct HashingCRF : CreateRegularFileSink
            {
                CreateRegularFileSink & inner;
                AsyncFileHasher & hasher;

                HashingCRF(CreateRegularFileSink & inner, AsyncFileHasher & hasher)
                    : inner(inner)
                    , hasher(hasher)
                {
                }

                void isExecutable() override
                {
                    /* the parser reports this before contents,
                       matching dump order */
                    hasher.fileExec();
                    inner.isExecutable();
                }

                void preallocateContents(uint64_t s) override
                {
                    hasher.fileSize(s);
                    inner.preallocateContents(s);
                }

                void operator()(std::string_view data) override
                {
                    hasher.fileData(data);
                    inner(data);
                }
            } hcrf{crf, hasher};
            hasher.fileBegin((prefix / path).abs());
            func(hcrf);
        });
        /* End goes here, NOT inside the callback: RestoreSink does its
           flush, fchmod and futimens after the callback returns, so
           queueing End from inside let the hasher's streamed dedup
           link+rename over a file whose tail was still in the FdSink
           buffer. That was survivable only because everything after
           func() is fd-relative and so landed harmlessly on the
           orphaned inode. Out here the file really is finished, which
           is what tryDedup's comment claims. */
        hasher.fileEnd();
    }
};

/* restorePath(), optionally capturing per-file hashes. */
static void restorePathCapturingHashes(
    const std::filesystem::path & path,
    Source & source,
    bool startFsync,
    LocalStore::ImportFileHashes * fileHashes,
    const std::filesystem::path * linksDir,
    bool canonical,
    size_t dedupThreads)
{
    if (!fileHashes) {
        restorePath(path, source, startFsync, canonical);
        return;
    }
    RestoreSink inner{startFsync, canonical};
    inner.dstPath = path;
    /* directory canonicalisation must wait for the hasher: its
       streamed dedup swaps files via link+rename, which bumps the
       just-stamped mtime of the containing directory. Children land
       in the list before parents, the root last (finishCanonical
       below). */
    std::vector<CanonPath> dirs;
    if (canonical)
        inner.deferCanonicalDirs = &dirs;
    AsyncFileHasher hasher{*fileHashes, linksDir ? &path : nullptr, linksDir, dedupThreads};
    FileHashingSink sink{inner, CanonPath::root, hasher};
    parseDump(sink, source);
    hasher.finish();
    /* the root is deliberately NOT finished here: a directory rename
       across parents (moveFile out of the temp dir) needs write
       permission on the moved directory itself, so the caller
       canonicalises the root once it reaches its final path */
    /* through the sink's root descriptor, which is still open: a bare
       chmod(path) was the one write in an import that followed symlinks
       (every openat2 here carries RESOLVE_NO_SYMLINKS|RESOLVE_BENEATH),
       and this costs the same two syscalls it did. */
    for (auto & dir : dirs) {
        fchmodatTryNoFollow(inner.dirFd.get(), dir, 0755);
        setWriteTimeAt(inner.dirFd.get(), dir, mtimeStore, mtimeStore);
    }
}

StorePath
LocalStore::addToStoreFromDump(Source & source0, std::string_view name)
{
    return addToStoreFromDump(source0, name, nullptr);
}

StorePath LocalStore::addToStoreFromDump(
    Source & source0, std::string_view name, ImportFileHashes * fileHashes)
{
    /* For computing the store path; hashed off-thread so it overlaps
       with the restore below. */
    auto hashSink = std::make_unique<AsyncHashSink>();
    TeeSource source{source0, *hashSink};
    const LocalSettings & localSettings = config->getLocalSettings();

    /* Every dump goes through a temporary path in the store: it is
       restored there, hashed as it streams, and moved into place once
       the hash names it. The old in-memory shortcut kept the first
       narBufferSize (32 MiB) of the dump in a heap buffer so a small
       NAR could skip the temp dir, but a generation is orders of
       magnitude larger than that, so the buffer was filled, spilled and
       thrown away on every real commit: 67% of the import's heap, and
       ~745 reallocs growing it. What it bought was skipping the restore
       when a small path turns out to be valid already; that case now
       restores into the temp dir and deletes it, which is what every
       large import has always done. */
    std::unique_ptr<AutoDelete> delTempDir;
    std::filesystem::path tempPath;
    std::filesystem::path tempDir;
    AutoCloseFD tempDirFd;

    /* NAR restores create every node themselves, so the sink can stamp
       store-canonical metadata as it goes and the canonicalise walk
       below becomes redundant; both restore targets (temp dir and real
       path) live under realStoreDir, so one ACL check covers them */
    bool canonicalRestore = !dirGrantsDefaultAcl(config->realStoreDir);

    std::tie(tempDir, tempDirFd) = createTempDirInStore();
    delTempDir = std::make_unique<AutoDelete>(tempDir);
    tempPath = tempDir / "x";

    restorePathCapturingHashes(
        tempPath,
        source,
        localSettings.fsyncStorePaths,
        fileHashes,
        &linksDir,
        canonicalRestore,
        localSettings.dedupThreads);

    /* The dump is a NAR hashed with SHA-256, which is exactly the
       store path's content address: no second pass over the restored
       tree to hash it a different way. */
    auto [dumpHash, size] = hashSink->finish();

    auto dstPath = makeContentAddressedPath(name, dumpHash);


    if (!isValidPath(dstPath)) {

        /* The first check above is an optimisation to prevent
           unnecessary lock acquisition. */

        auto realPath = toRealPath(dstPath);

        PathLocks outputLock({realPath});

        /* The path may have been created by another process in the meantime, so check again. */
        if (!isValidPath(dstPath)) {

            deletePath(realPath);


            /* Move the temporary path we restored above. */
            moveFile(tempPath, realPath);

            /* merged into restorePath: the canonical restore stamps
               every node as it is created, so the full walk (lstat +
               llistxattr + chmod + utimensat per entry, half of them
               journaled writes on the store medium) only runs for the
               layouts the sink cannot cover. The root is the one node
               the sink left alone (the move above needed it writable);
               one lstat + chmod + utimensat finishes it in place */
            if (canonicalRestore)
                canonicaliseTimestampAndPermissions(realPath);
            else
                canonicalisePathMetaData(
                    realPath, {NIX_WHEN_SUPPORT_ACLS(localSettings.ignoredAcls)});

            optimisePath(realPath);

            if (localSettings.fsyncStorePaths) {
                recursiveSync(realPath);
                syncParent(realPath);
            }

            auto info = ValidPathInfo::makeFromCA(*this, name, dumpHash);
            info.narSize = size;
            registerValidPath(info);
        }

        outputLock.setDeletion(true);
    }

    return dstPath;
}

/* Create a temporary directory in the store that won't be
   garbage-collected until the returned FD is closed. */
std::pair<std::filesystem::path, AutoCloseFD> LocalStore::createTempDirInStore()
{
    std::filesystem::path tmpDirFn;
    AutoCloseFD tmpDirFd;
    bool lockedByUs = false;
    /* The retry exists for one race (the GC deleting the dir between
       creating and locking it), which resolves on the next pass. Any
       other failure is persistent, and an unbounded `continue` on it
       spins a core forever instead of reporting: a read-only store,
       EMFILE, or a store dir we cannot enter all reach here. Bound it,
       and let the last errno explain. */
    for (unsigned attempt = 0;; attempt++) {
        if (attempt == 64)
            throw SysError("creating a temporary directory in %1%",
                PathFmt(config->realStoreDir));
        tmpDirFn = createTempDir(std::filesystem::path{config->realStoreDir}, "tmp", /*mode=*/0700);
        tmpDirFd = openDirectory(tmpDirFn, FinalSymlink::DontFollow);
        if (!tmpDirFd)
            continue;
        lockedByUs = lockFile(tmpDirFd.get(), ltWrite, true);
        if (pathExists(tmpDirFn) && lockedByUs)
            break;
    }
    return {tmpDirFn, std::move(tmpDirFd)};
}

void PathInUse::anchor() {}

void LocalStore::invalidatePathChecked(const StorePath & path)
{
    retrySQLite<void>([&]() {
        auto state(_state->lock());

        SQLiteTxn txn(state->db);

        if (isValidPath_(*state, path)) {
            StorePathSet referrers;
            queryReferrers(*state, path, referrers);
            referrers.erase(path); /* ignore self-references */
            if (!referrers.empty())
                throw PathInUse(
                    "cannot delete path '%s' because it is in use by %s",
                    printStorePath(path),
                    concatMapStringsSep(", ", referrers, [&](auto & p) { return "'" + printStorePath(p) + "'"; }));
            invalidatePath(*state, path);
        }

        txn.commit();
    });
}

/* Detect, never heal: repairPath went with the substituter
   machinery, so there is nothing to heal a bad path from. */
bool LocalStore::verifyStore(bool checkContents)
{
    printInfo("reading the Nix store...");

    /* Acquire the global GC lock to get a consistent snapshot of
       existing and valid paths. */
    auto fdGCLock = openGCLock();
    FdLock gcLock(fdGCLock.get(), ltRead, true, "waiting for the big garbage collector lock...");

    auto [errors, validPaths] = verifyAllValidPaths();

    /* Optionally, check the content hashes (slow). */
    if (checkContents) {

        printInfo("checking link hashes...");

        for (auto & link : DirectoryIterator{linksDir}) {
            checkInterrupt();
            auto name = link.path().filename();
            printMsg(lvlTalkative, "checking contents of %s", PathFmt(name));
            std::string hash =
                hashPath(makeFSSourceAccessor(link.path()))
                    .hash.to_string(HashFormat::Nix32, false);
            if (hash != name.string()) {
                printError(
                    "link %s was modified! expected hash %s, got '%s'", PathFmt(link.path()), name.string(), hash);
                errors = true;
            }
        }

        printInfo("checking store hashes...");

        Hash nullHash;

        for (auto & i : validPaths) {
            try {
                auto info =
                    std::const_pointer_cast<ValidPathInfo>(std::shared_ptr<const ValidPathInfo>(queryPathInfo(i)));

                /* Check the content hash (optionally - slow). */
                printMsg(lvlTalkative, "checking contents of '%s'", printStorePath(i));

                auto hashSink = HashSink();

                dumpPath(toRealPath(i), hashSink);
                auto current = hashSink.finish();

                if (info->narHash != nullHash && info->narHash != current.hash) {
                    printError(
                        "path '%s' was modified! expected hash '%s', got '%s'",
                        printStorePath(i),
                        info->narHash.to_string(HashFormat::Nix32, true),
                        current.hash.to_string(HashFormat::Nix32, true));
                    errors = true;
                } else {

                    bool update = false;

                    /* Fill in missing hashes. */
                    if (info->narHash == nullHash) {
                        printInfo("fixing missing hash on '%s'", printStorePath(i));
                        info->narHash = current.hash;
                        update = true;
                    }

                    /* Fill in missing narSize fields (from old stores). */
                    if (info->narSize == 0) {
                        printInfo("updating size field on '%s' to %s", printStorePath(i), current.numBytesDigested);
                        info->narSize = current.numBytesDigested;
                        update = true;
                    }

                    if (update)
                        updatePathInfo(*_state->lock(), *info);
                }

            } catch (Error & e) {
                /* It's possible that the path got GC'ed, so ignore
                   errors on invalid paths. */
                if (isValidPath(i))
                    logError(e.info());
                else
                    logWarning(e.info());
                errors = true;
            }
        }
    }

    return errors;
}

LocalStore::VerificationResult LocalStore::verifyAllValidPaths()
{
    StorePathSet storePathsInStoreDir;
    /* Why aren't we using `queryAllValidPaths`? Because that would
       tell us about all the paths than the database knows about. Here we
       want to know about all the store paths in the store directory,
       regardless of what the database thinks.

       We will end up cross-referencing these two sources of truth (the
       database and the filesystem) in the loop below, in order to catch
       invalid states.
     */
    for (auto & i : DirectoryIterator{config->realStoreDir}) {
        checkInterrupt();
        try {
            storePathsInStoreDir.insert({i.path().filename().string()});
        } catch (BadStorePath &) {
        }
    }

    /* Check whether all valid paths actually exist. */
    printInfo("checking path existence...");

    StorePathSet done;

    auto existsInStoreDir = [&](const StorePath & storePath) { return storePathsInStoreDir.count(storePath); };

    bool errors = false;
    StorePathSet validPaths;

    for (auto & i : queryAllValidPaths())
        verifyPath(i, existsInStoreDir, done, validPaths, errors);

    return {
        .errors = errors,
        .validPaths = validPaths,
    };
}

void LocalStore::verifyPath(
    const StorePath & path,
    fun<bool(const StorePath &)> existsInStoreDir,
    StorePathSet & done,
    StorePathSet & validPaths,
    bool & errors)
{
    checkInterrupt();

    if (!done.insert(path).second)
        return;

    if (!existsInStoreDir(path)) {
        /* Walk referrers too, so one pass names everything that went
           with it rather than stopping at the first casualty. */
        StorePathSet referrers;
        queryReferrers(path, referrers);
        for (auto & i : referrers)
            if (i != path)
                verifyPath(i, existsInStoreDir, done, validPaths, errors);

        /* Report, never heal: dropping the registration here would
           destroy the very record
           the operator ran verify to find: the basename is what a GRUB
           entry and a gcroot name, and reporting "consistent"
           afterwards would be a lie. Prune deliberately with rm-path
           once you have seen what vanished. */
        printError("path '%s' disappeared from the store directory", printStorePath(path));
        errors = true;

        return;
    }

    validPaths.insert(std::move(path));
}


ref<LocalStore> openStore(const std::filesystem::path & root, bool mustExist)
{
    if (!root.is_absolute())
        throw UsageError("store root '%s' must be an absolute path", root.string());
    auto config = make_ref<LocalStore::Config>(root);
    /* Checked before the store is constructed: the constructor creates
       the store and state directories, so by the time it could report
       an empty store it has already made one. */
    if (mustExist && !std::filesystem::exists(config->stateDir / "db" / "db.sqlite"))
        throw Error("no store at '%s': no database", root.string());
    return config->openStore();
}

} // namespace nix
