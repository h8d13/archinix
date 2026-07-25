#include "nix/store/gc-store.hh"
#include "nix/store/local-settings.hh"
#include "nix/store/local-store.hh"
#include "nix/store/path.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/finally.hh"
#include "nix/util/signals.hh"
#include "nix/util/serialise.hh"
#include "nix/util/util.hh"
#include "nix/util/file-system.hh"
#include "nix/store/posix-fs-canonicalise.hh"

#include "store-config-private.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <queue>
#include <thread>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <variant>
#if HAVE_STATVFS
#  include <sys/statvfs.h>
#endif
#  include <poll.h>
#  include <sys/socket.h>
#  include <sys/un.h>
#include <sys/types.h>
#include <unistd.h>

namespace nix {

static std::string gcRootsDir = "gcroots";

void LocalStore::makeSymlink(const std::filesystem::path & link, const std::filesystem::path & target)
{
    /* Create directories up to `gcRoot'. */
    createDirs(link.parent_path());

    /* Create the new symlink. */
    auto tempLink = std::filesystem::path(link) += fmt(".tmp-%1%-%2%", getpid(), rand());
    createSymlink(target, tempLink);

    /* Atomically replace the old one. */
    std::filesystem::rename(tempLink, link);
}

std::filesystem::path LocalStore::addPermRoot(const StorePath & storePath, const std::filesystem::path & _gcRoot)
{
    auto gcRoot = canonPath(_gcRoot);

    if (isInStore(gcRoot.string()))
        throw Error(
            "creating a garbage collector root (%1%) in the Nix store is forbidden "
            "(are you running nix-build inside the store?)",
            PathFmt(gcRoot));

    /* Register this root with the garbage collector, if it's
       running. This should be superfluous since the caller should
       have registered this root yet, but let's be on the safe
       side. */

    /* Don't clobber the link if it already exists and doesn't
       point to the Nix store. */
    if (pathExists(gcRoot) && (!std::filesystem::is_symlink(gcRoot) || !isInStore(readLink(gcRoot).string())))
        throw Error("cannot create symlink %1%; already exists", PathFmt(gcRoot));

    makeSymlink(gcRoot, printStorePath(storePath));

    return gcRoot;
}

void LocalStore::findRoots(const std::filesystem::path & path, std::filesystem::file_type type, Roots & roots)
{
    auto foundRoot = [&](const std::filesystem::path & path, const std::filesystem::path & target) {
        try {
            auto storePath = toStorePath(target.string()).first;
            if (isValidPath(storePath))
                roots[std::move(storePath)].emplace(path.string());
            else
                printInfo("skipping invalid root from %1% to %2%", PathFmt(path), PathFmt(target));
        } catch (BadStorePath &) {
        }
    };

    try {

        if (type == std::filesystem::file_type::unknown)
            type = std::filesystem::symlink_status(path).type();

        if (type == std::filesystem::file_type::directory) {
            for (auto & i : DirectoryIterator{path}) {
                checkInterrupt();
                findRoots(i.path(), i.symlink_status().type(), roots);
            }
        }

        else if (type == std::filesystem::file_type::symlink) {
            auto target = readLink(path);
            if (isInStore(target.string()))
                foundRoot(path, target);

            /* Handle indirect roots. */
            else {
                auto parentPath = path.parent_path();
                target = absPath(target, &parentPath);
                if (!pathExists(target)) {
                    if (isInDir(path, config->stateDir / gcRootsDir / "auto")) {
                        printInfo("removing stale link from %1% to %2%", PathFmt(path), PathFmt(target));
                        tryUnlink(path);
                    }
                } else {
                    if (!std::filesystem::is_symlink(target))
                        return;
                    auto target2 = readLink(target);
                    if (isInStore(target2.string()))
                        foundRoot(target, target2);
                }
            }
        }

        else if (type == std::filesystem::file_type::regular) {
            auto storePath = maybeParseStorePath(storeDir + "/" + std::string(baseNameOf(path.string())));
            if (storePath && isValidPath(*storePath))
                roots[std::move(*storePath)].emplace(path.string());
        }

    }

    catch (std::filesystem::filesystem_error & e) {
        /* We only ignore permanent failures. */
        if (e.code() == std::errc::permission_denied || e.code() == std::errc::no_such_file_or_directory
            || e.code() == std::errc::not_a_directory)
            printInfo("cannot read potential root %1%", PathFmt(path));
        else
            throw SystemError(e.code(), "finding GC roots in %1%", PathFmt(path));
    }

    catch (SystemError & e) {
        /* We only ignore permanent failures. */
        if (e.is(std::errc::permission_denied) || e.is(std::errc::no_such_file_or_directory)
            || e.is(std::errc::not_a_directory))
            printInfo("cannot read potential root %1%", PathFmt(path));
        else
            throw;
    }
}

void LocalStore::findRootsNoTemp(Roots & roots)
{
    /* Process direct roots in {gcroots,profiles}. */
    findRoots(config->stateDir / gcRootsDir, std::filesystem::file_type::unknown, roots);
    findRoots(config->stateDir / "profiles", std::filesystem::file_type::unknown, roots);
}

struct GCLimitReached
{};

void LocalStore::collectGarbage(const GCOptions & options, GCResults & results)
{
    bool shouldDelete = options.action == GCOptions::gcDeleteDead || options.action == GCOptions::gcDeleteSpecific;

    boost::unordered_flat_set<StorePath, std::hash<StorePath>> roots, dead, alive;

    /* Return early if nothing to delete */
    if (std::visit(
            overloaded{
                [](const GCOptions::SpecificPaths & pathsToDelete) { return pathsToDelete.paths.empty(); },
                [](const GCOptions::WholeStore & _) { return false; }},
            options.pathsToDelete))
        return;

    if (shouldDelete)
        deletePath(reservedPath);

    /* Acquire the global GC root. Note: we don't use fdGCLock
       here because then in auto-gc mode, another thread could
       downgrade our exclusive lock. */
    auto fdGCLock = openGCLock();
    FdLock gcLock(fdGCLock.get(), ltWrite, true, "waiting for the big garbage collector lock...");

    /* Find the roots.  Since we've grabbed the GC lock, the set of
       permanent roots cannot increase now. */
    printInfo("finding garbage collector roots...");
    Roots rootMap;
    if (!options.ignoreLiveness)
        findRootsNoTemp(rootMap);

    for (auto & i : rootMap)
        roots.insert(i.first);

    /* Synchronisation point for testing, see tests/functional/gc-non-blocking.sh. */
    if (auto p = getEnv("_NIX_TEST_GC_SYNC_2"))
        readFile(*p);

    /* Helper function that deletes a path from the store and throws
       GCLimitReached if we've deleted enough garbage. */
    auto deleteFromStore = [&](std::string_view baseName, bool isKnownPath) {
        assert(!std::filesystem::path(baseName).is_absolute());
        /* Using `std::string` since this is the logical store dir. Hopefully that is the right choice. */
        std::string path = storeDir + "/" + std::string(baseName);
        auto realPath = config->realStoreDir / std::string(baseName);

        /* There may be temp directories in the store that are still in use
           by another process. We need to be sure that we can acquire an
           exclusive lock before deleting them. */
        if (baseName.find("tmp-", 0) == 0) {
            /* TODO Reconsider whether Follow is the right choice, here */
            auto tmpDirFd = openDirectory(realPath, FinalSymlink::Follow);
            if (!tmpDirFd || !lockFile(tmpDirFd.get(), ltWrite, false)) {
                debug("skipping locked tempdir %s", PathFmt(realPath));
                return;
            }
        }

        printInfo("deleting '%1%'", path);

        results.paths.insert(path);

        uint64_t bytesFreed;
        deleteStorePath(realPath, bytesFreed, isKnownPath);

        results.bytesFreed += bytesFreed;

        if (results.bytesFreed > options.maxFreed) {
            printInfo("deleted more than %d bytes; stopping", options.maxFreed);
            throw GCLimitReached();
        }
    };

    boost::unordered_flat_map<StorePath, StorePathSet, std::hash<StorePath>> referrersCache;

    /* Helper function that visits all paths reachable from `start`
       via the referrers edges and optionally derivers and derivation
       output edges. If none of those paths are roots, then all
       visited paths are garbage and are deleted. */
    auto maybeDeleteReferrersClosure = [&](const StorePath & start) {
        StorePathSet visited;
        std::queue<StorePath> todo;

        auto enqueue = [&](const StorePath & path) {
            if (visited.insert(path).second)
                todo.push(path);
        };

        /* A generation is an independent, self-contained tree:
           import-dir creates them reference-free and import-path
           refuses a stream that claims references, so a path's closure
           is the path itself. The Refs table and its `on delete
           restrict` FK stay as the backstop: if that door check were
           ever bypassed, deleting a referenced path fails loudly
           instead of losing it silently. */
        auto markAlive = [&](const StorePath & p) { alive.insert(p); };

        enqueue(start);

        while (auto path = pop(todo)) {
            checkInterrupt();

            /* Bail out if we've previously discovered that this path
               is alive. */
            if (alive.contains(*path)) {
                debug("cannot delete '%s' because '%s' is alive", printStorePath(start), printStorePath(*path));
                alive.insert(start);
                return;
            }

            /* If we've previously deleted this path, we don't have to
               handle it again. */
            if (dead.contains(*path))
                continue;

            /* If this is a root, bail out. */
            if (roots.contains(*path)) {
                debug("cannot delete '%s' because it's a root", printStorePath(*path));
                alive.insert(start);
                return markAlive(*path);
            }

            if (std::visit(
                    overloaded{
                        [&](const GCOptions::SpecificPaths & pathsToDelete) {
                            if (!pathsToDelete.deleteReferrers && !pathsToDelete.paths.contains(*path)) {
                                debug(
                                    "cannot delete '%s' because '%s' is not in the specified paths to delete",
                                    printStorePath(start),
                                    printStorePath(*path));
                                return true;
                            }
                            return false;
                        },
                        [](const GCOptions::WholeStore & _) { return false; },
                    },
                    options.pathsToDelete))
                return;

            if (isValidPath(*path)) {

                /* Visit the referrers of this path. */
                auto i = referrersCache.find(*path);
                if (i == referrersCache.end()) {
                    StorePathSet referrers;
                    queryGCReferrers(*path, referrers);
                    referrersCache.emplace(*path, std::move(referrers));
                    i = referrersCache.find(*path);
                }
                for (auto & p : i->second)
                    enqueue(p);
            }
        }
        for (auto & path : topoSortPaths(visited)) {
            if (!dead.insert(path).second)
                continue;
            if (shouldDelete) {
                try {
                    invalidatePathChecked(path);
                    deleteFromStore(path.to_string(), true);
                    referrersCache.erase(path);
                } catch (PathInUse & e) {
                    // If we end up here, it's likely a new occurrence
                    // of https://github.com/NixOS/nix/issues/11923
                    printError("BUG: %s", e.what());
                }
            }
        }
    };

    try {
        /* Either delete all garbage paths, or just the specified paths. */
        std::visit(
            overloaded{
                [&](const GCOptions::SpecificPaths & pathsToDelete) {
                    switch (options.action) {
                    case GCOptions::gcDeleteDead:
                        printInfo("deleting garbage within specified paths...");
                        break;
                    case GCOptions::gcDeleteSpecific:
                        printInfo("deleting specified paths...");
                        break;
                    case GCOptions::gcReturnDead:
                    case GCOptions::gcReturnLive:
                        printInfo("determining live/dead paths...");
                    }

                    for (auto & i : pathsToDelete.paths) {
                        maybeDeleteReferrersClosure(i);

                        if (options.action == GCOptions::gcDeleteSpecific && !dead.contains(i))
                            throw Error(
                                "Cannot delete path '%1%' since it is still alive. "
                                "To find out why, use: "
                                "nix-store --query --roots and nix-store --query --referrers",
                                printStorePath(i));
                        else if (!dead.contains(i))
                            debug("cannot delete '%s' because it's still alive", printStorePath(i));
                    }
                },
                [&](const GCOptions::WholeStore & _) {
                    if (options.maxFreed == 0)
                        return;

                    switch (options.action) {
                    case GCOptions::gcDeleteDead:
                        printInfo("deleting garbage...");
                        break;
                    case GCOptions::gcDeleteSpecific:
                        throw Error("Cannot delete the entire store");
                    case GCOptions::gcReturnDead:
                    case GCOptions::gcReturnLive:
                        printInfo("determining live/dead paths...");
                    }

                    AutoCloseDir dir(opendir(config->realStoreDir.string().c_str()));
                    if (!dir)
                        throw SysError("opening directory %1%", PathFmt(config->realStoreDir));

                    /* Read the store and delete all paths that are invalid or
                    unreachable. We don't use readDirectory() here so that
                    GCing can start faster. */
                    auto linksName = linksDir.filename();
                    struct dirent * dirent;
                    while (errno = 0, dirent = readdir(dir.get())) {
                        checkInterrupt();
                        std::string name = dirent->d_name;
                        if (name == "." || name == ".." || name == linksName)
                            continue;

                        if (auto storePath = maybeParseStorePath(storeDir + "/" + name))
                            maybeDeleteReferrersClosure(*storePath);
                        else
                            deleteFromStore(name, false);
                    }
                },
            },
            options.pathsToDelete);
    } catch (GCLimitReached & e) {
    }

    if (options.action == GCOptions::gcReturnLive) {
        for (auto & i : alive)
            results.paths.insert(printStorePath(i));
        return;
    }

    if (options.action == GCOptions::gcReturnDead) {
        for (auto & i : dead)
            results.paths.insert(printStorePath(i));
        return;
    }

    /* Unlink all files in /nix/store/.links that have a link count of 1,
       which indicates that there are no other links and so they can be
       safely deleted.  FIXME: race condition with optimisePath(): we
       might see a link count of 1 just before optimisePath() increases
       the link count. */
    if (options.action == GCOptions::gcDeleteDead || options.action == GCOptions::gcDeleteSpecific) {
        printInfo("deleting unused links...");

        AutoCloseDir dir(opendir(linksDir.string().c_str()));
        if (!dir)
            throw SysError("opening directory %1%", PathFmt(linksDir));

        int64_t actualSize = 0, unsharedSize = 0;

        struct dirent * dirent;
        while (errno = 0, dirent = readdir(dir.get())) {
            checkInterrupt();
            std::string name = dirent->d_name;
            if (name == "." || name == "..")
                continue;
            auto path = linksDir / name;

            auto st = lstat(path);

            if (st.st_nlink != 1) {
                actualSize += st.st_size;
                unsharedSize += (st.st_nlink - 1) * st.st_size;
                continue;
            }

            printMsg(lvlTalkative, "deleting unused link %1%", PathFmt(path));

            unlink(path);

            /* Do not account for deleted file here. Rely on deletePath()
               accounting.  */
        }

        int64_t overhead =
            [&] {
                auto st = stat(linksDir);
                return st.st_blocks * 512ULL;
            }()
            ;

        printInfo("note: hard linking is currently saving %s", renderSize(unsharedSize - actualSize - overhead));
    }

    /* Deliberately no VACUUM here: a generation is one ValidPaths row,
       so the db stays tiny (60 generations measured 52 KB, of which a
       vacuum reclaims 16 KB) while the paths it describes are
       gigabytes. Rewriting the whole db to save that is not a trade
       worth making on the store disk. */
}

} // namespace nix
