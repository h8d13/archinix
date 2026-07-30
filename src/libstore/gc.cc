#include "nix/store/gc-store.hh"
#include "nix/store/local-store.hh"
#include "nix/store/path.hh"
#include "nix/util/signals.hh"
#include "nix/util/file-system.hh"

#include <errno.h>
#include <sys/stat.h>
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

/* A root is a symlink into the store, made by addPermRoot and nothing
   else: the indirect (`gcroots/auto`) layer and the regular-file root
   are both gone, so a link that does not resolve into the store is
   simply not a root. */
void LocalStore::findRoots(const std::filesystem::path & path, std::filesystem::file_type type, Roots & roots)
{
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
            if (!isInStore(target.string()))
                return;
            try {
                auto storePath = toStorePath(target.string()).first;
                if (isValidPath(storePath))
                    roots.insert(std::move(storePath));
            } catch (BadStorePath &) {
            }
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

/* Deleting named paths is the only collection this store does. There
   is no whole-store sweep and no live/dead query: rm-path names what
   goes, and what it names is either rooted (refused) or gone. */
void LocalStore::collectGarbage(const GCOptions & options, GCResults & results)
{
    if (options.paths.empty())
        return;

    deletePath(reservedPath);

    /* Acquire the global GC root. Note: we don't use fdGCLock
       here because then in auto-gc mode, another thread could
       downgrade our exclusive lock. */
    auto fdGCLock = openGCLock();
    FdLock gcLock(fdGCLock.get(), ltWrite, true, "waiting for the big garbage collector lock...");

    /* Find the roots.  Since we've grabbed the GC lock, the set of
       permanent roots cannot increase now. */
    Roots roots;
    findRoots(config->stateDir / gcRootsDir, std::filesystem::file_type::unknown, roots);

    auto deleteFromStore = [&](std::string_view baseName) {
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

        results.paths.insert(path);

        uint64_t bytesFreed;
        deleteStorePath(realPath, bytesFreed);

        results.bytesFreed += bytesFreed;
    };

    /* A generation is an independent, self-contained tree: import-dir
       creates them reference-free and import-path refuses a stream
       that claims references, so a path's closure is the path itself.
       That is why this is one decision per path and not a graph walk:
       no referrer edges to follow, so nothing else can be dragged in
       or held alive by the path under consideration. The Refs table
       and its `on delete restrict` FK stay as the backstop: bypass the
       door and invalidatePathChecked throws PathInUse instead of data
       being lost silently. */
    for (auto & path : options.paths) {
        checkInterrupt();

        if (roots.contains(path))
            throw Error(
                "cannot delete path '%1%' since it is still a garbage "
                "collector root",
                printStorePath(path));

        try {
            invalidatePathChecked(path);
            deleteFromStore(path.to_string());
        } catch (PathInUse & e) {
            // If we end up here, it's likely a new occurrence
            // of https://github.com/NixOS/nix/issues/11923
            printError("BUG: %s", e.what());
        }
    }

    /* Unlink all files in /nix/store/.links that have a link count of 1,
       which indicates that there are no other links and so they can be
       safely deleted.  FIXME: race condition with optimisePath(): we
       might see a link count of 1 just before optimisePath() increases
       the link count. */
    AutoCloseDir dir(opendir(linksDir.string().c_str()));
    if (!dir)
        throw SysError("opening directory %1%", PathFmt(linksDir));

    struct dirent * dirent;
    while (errno = 0, dirent = readdir(dir.get())) {
        checkInterrupt();
        std::string name = dirent->d_name;
        if (name == "." || name == "..")
            continue;
        auto path = linksDir / name;

        if (lstat(path).st_nlink != 1)
            continue;

        unlink(path);

        /* Do not account for deleted file here. Rely on deletePath()
           accounting.  */
    }

    /* Deliberately no VACUUM here: a generation is one ValidPaths row,
       so the db stays tiny (60 generations measured 52 KB, of which a
       vacuum reclaims 16 KB) while the paths it describes are
       gigabytes. Rewriting the whole db to save that is not a trade
       worth making on the store disk. */
}

} // namespace nix
