#include "nix/store/local-store.hh"
#include "nix/util/archive.hh"
#include "nix/util/thread-pool.hh"
#include "nix/store/local-settings.hh"
#include "nix/util/finally.hh"
#include "nix/util/signals.hh"
#include "nix/store/posix-fs-canonicalise.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/file-system.hh"
#include "nix/util/progress.hh"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <random>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "store-config-private.hh"

namespace nix {

static void makeWritable(const std::filesystem::path & path)
{
    auto st = lstat(path);
    /* dirs canonicalise to 0755 (posix-fs-canonicalise.cc), which
       already carries S_IWUSR: without this guard the chmod writes back
       the mode it just read, once per directory of every optimise walk.
       Not deletable: a store written before the 0755 decision has 0555
       dirs. */
    if (!(st.st_mode & S_IWUSR))
        chmod(path, st.st_mode | S_IWUSR);
}

struct MakeReadOnly
{
    std::filesystem::path path;

    MakeReadOnly(std::filesystem::path path)
        : path(std::move(path))
    {
    }

    ~MakeReadOnly()
    {
        try {
            /* This will make the path read-only. */
            if (!path.empty())
                canonicaliseTimestampAndPermissions(path.string());
        } catch (...) {
            /* Cannot throw from here, but the consequence is a
               directory left writable inside a store path, which is
               precisely the state optimisePath_ warns about for files
               and would otherwise pass in silence. Say so. */
            try {
                printError("optimise: %s left writable: its mode could not be restored",
                    PathFmt(path));
            } catch (...) {
            }
            ignoreExceptionInDestructor();
        }
    }
};

LocalStore::InodeHash LocalStore::loadInodeHash()
{
    debug("loading hash inodes in memory");
    InodeHash inodeHash;

    AutoCloseDir dir(opendir(linksDir.string().c_str()));
    if (!dir)
        throw SysError("opening directory %1%", PathFmt(linksDir));

    struct dirent * dirent;
    while (errno = 0, dirent = readdir(dir.get())) { /* sic */
        checkInterrupt();
        // We don't care if we hit non-hash files, anything goes
        inodeHash.insert(dirent->d_ino);
    }
    if (errno)
        throw SysError("reading directory %1%", PathFmt(linksDir));

    printMsg(lvlTalkative, "loaded %1% hash inodes", inodeHash.size());

    return inodeHash;
}

std::vector<LocalStore::OptimiseEnt>
LocalStore::readDirectoryIgnoringInodes(const std::filesystem::path & path, const InodeHash & inodeHash)
{
    std::vector<OptimiseEnt> names;

    AutoCloseDir dir(opendir(path.string().c_str()));
    if (!dir)
        throw SysError("opening directory %s", PathFmt(path));

    struct dirent * dirent;
    while (errno = 0, dirent = readdir(dir.get())) { /* sic */
        checkInterrupt();

        if (inodeHash.contains(dirent->d_ino)) {
            debug("'%1%' is already linked", dirent->d_name);
            continue;
        }

        std::string name = dirent->d_name;
        if (name == "." || name == "..")
            continue;
        /* DT_UNKNOWN (some filesystems never fill d_type in) falls
           through as "not a directory": the walk then recurses inline
           instead of forking a task, which is the old behaviour and
           still correct, just not parallel. */
        names.emplace_back(std::move(name), dirent->d_type == DT_DIR);
    }
    if (errno)
        throw SysError("reading directory %s", PathFmt(path));

    return names;
}

void LocalStore::optimisePath_(
    OptimiseCtx & ctx,
    const std::filesystem::path & path,
    bool * parentToggled,
    ThreadPool * pool)
{
    checkInterrupt();

    auto st = lstat(path);

    if (S_ISDIR(st.st_mode)) {
        auto names = readDirectoryIgnoringInodes(path, ctx.inodeHash);

        /* The first child that relinks makes this directory writable;
           restore it once after all children instead of per file.
           Only this frame's own file children touch the flag: child
           directories are walked by their own frame (or their own
           task), each with a flag of its own, so the toggle stays
           single-writer however the walk is scheduled. */
        bool toggled = false;
        Finally restore([&]() {
            if (toggled) {
                try {
                    canonicaliseTimestampAndPermissions(path);
                } catch (...) {
                    /* see ~MakeReadOnly: a silently writable directory
                       in a store path is the failure worth hearing
                       about, even though we cannot throw from here */
                    try {
                        printError("optimise: %s left writable: its mode could not be restored",
                            PathFmt(path));
                    } catch (...) {
                    }
                    ignoreExceptionInDestructor();
                }
            }
        });
        for (auto & i : names) {
            /* Subtrees are disjoint, so each is a task. Restoring this
               directory's mode does not have to wait for them: a file
               only ever needs its own immediate parent writable, and
               that parent is inside the subtree the task owns. */
            if (pool && i.isDir) {
                auto child = path / i.name;
                pool->enqueue([this, &ctx, child, pool] {
                    optimisePath_(ctx, child, nullptr, pool);
                });
            } else
                optimisePath_(ctx, path / i.name, &toggled, pool);
        }
        return;
    }

    /* We can hard link regular files and maybe symlinks. */
    if (!S_ISREG(st.st_mode)
#if CAN_LINK_SYMLINK
        && !S_ISLNK(st.st_mode)
#endif
    )
        return;

    /* Everything past the gate above is a node this pass considers,
       symlinks included: they are most of a store tree and most of
       what gets linked, so counting only regular files would track a
       fraction of the work. Files the import already hard-linked
       never arrive (readDirectoryIgnoringInodes drops them by inode),
       which is what makes this pass cheap after a mostly-unchanged
       import, so the total nets those out. */
    progressTick("optimising", ctx.visited.fetch_add(1, std::memory_order_relaxed) + 1, ctx.total);

    /* Sometimes SNAFUs can cause files in the Nix store to be
       modified, in particular when running programs as root under
       NixOS (example: $fontconfig/var/cache being modified).  Skip
       those files.  FIXME: check the modification time. */
    if (S_ISREG(st.st_mode) && (st.st_mode & S_IWUSR)) {
        warn("skipping suspicious writable file '%s'", PathFmt(path));
        return;
    }

    /* Never link empty files: saves zero bytes and welds unrelated
       runtime-mutable paths (subuid, wtmp, lastlog, ...) into one
       inode, which a rootfs generation booted as an overlay lower
       layer then writes through. Also the classic too-many-links
       case. */
    if (S_ISREG(st.st_mode) && st.st_size == 0)
        return;

    /* This can still happen on top-level files. */
    if (st.st_nlink > 1 && ctx.inodeHash.contains(st.st_ino)) {
        debug("%s is already linked, with %d other file(s)", PathFmt(path), st.st_nlink - 2);
        return;
    }

    /* Hash the file.  Note that hashPath() returns the hash over the
       NAR serialisation, which includes the execute bit on the file.
       Thus, executable and non-executable files with the same
       contents *won't* be linked (which is good because otherwise the
       permissions would be screwed up).

       Also note that if `path' is a symlink, then we're hashing the
       contents of the symlink (i.e. the result of readlink()), not
       the contents of the target (which may not even exist).

       An import that captured per-file hashes while restoring spares
       the content re-read; anything not in the map (concurrent
       changes) falls back to hashing from disk. Symlinks are in the
       map too: hashing one from disk costs an O_NOFOLLOW open that
       takes ELOOP, a reopen of the parent by absolute path, and a
       readlink, for a digest the restore already had the target for. */
    Hash hash = [&] {
        /* The capture happened during the restore, under the import's
           PathLocks; this pass runs after those are released, so a
           captured hash is only trustworthy while the node still looks
           exactly as the restore left it. Canonical means mtime 1, so
           anything else was touched afterwards and the captured digest
           may no longer describe the contents -- and the branch below
           would then publish it into the farm as a key, poisoning every
           later dedup against that hash. Cheap to check: st is already
           in hand. */
        if (ctx.fileHashes && st.st_mtime == mtimeStore) {
            auto rel = path.native().substr(ctx.fileHashesBase);
            if (rel.empty())
                rel = "/";
            if (auto it = ctx.fileHashes->files.find(rel); it != ctx.fileHashes->files.end())
                return it->second;
        }
        return hashPath(makeFSSourceAccessor(path)).hash;
    }();
    debug("%s has hash '%s'", PathFmt(path), hash.to_string(HashFormat::Nix32, true));

    /* Check if this is a known hash. Single component parse: this runs
       once per file. */
    std::filesystem::path linkPath{linksDir.native() + '/' + hash.to_string(HashFormat::Nix32, false)};

    auto stLink = maybeLstat(linkPath);

    /* Maybe delete the link, if it has been corrupted. */
    if (stLink) {
        if (st.st_size != stLink->st_size) {
            // XXX: Consider overwriting linkPath with our valid version.
            warn("removing corrupted link %s", PathFmt(linkPath));
            warn("There may be more corrupted paths; verify-store --content names them all");
            unlinkIfExists(linkPath);
            stLink.reset();
        }
    }

    if (!stLink) {
        /* Nope, create a hard link in the links directory. */
        try {
            std::filesystem::create_hard_link(path, linkPath);
            ctx.inodeHash.insert(st.st_ino);
            /* Our file is now the canonical copy in the links
               directory; nothing left to replace. */
            return;
        } catch (std::filesystem::filesystem_error & e) {
            if (e.code() == std::errc::file_exists) {
                /* Fall through if another process created ‘linkPath’ before
                   we did. */
                stLink = maybeLstat(linkPath);

                /* A concurrent garbage collection may have removed the
                   link again already. Skip optimising this path; a
                   later pass will dedup it. */
                if (!stLink)
                    return;
            }

            else if (e.code() == std::errc::no_space_on_device) {
                /* On ext4, that probably means the directory index is
                   full.  When that happens, it's fine to ignore it: we
                   just effectively disable deduplication of this
                   file.
                   */
                printInfo("cannot link %s to '%s': %s", PathFmt(linkPath), PathFmt(path), e.code().message());
                return;
            }

            else
                throw SystemError(e.code(), "creating hard link from %1% to %2%", PathFmt(linkPath), PathFmt(path));
        }
    }

    /* Yes!  We've seen a file with the same contents.  Replace the
       current file with a hard link to that file. */
    if (st.st_ino == stLink->st_ino) {
        debug("%1% is already linked to %2%", PathFmt(path), PathFmt(linkPath));
        return;
    }

    printMsg(lvlTalkative, "linking %1% to %2%", PathFmt(path), PathFmt(linkPath));

    /* Make the containing directory writable, but only if it's not
       the store itself (we don't want or need to mess with its
       permissions). Inside a directory recursion the parent toggles
       once for all its files and restores after; only a top-level
       call toggles (and restores) here. */
    MakeReadOnly makeReadOnly{std::filesystem::path{}};
    if (parentToggled) {
        if (!*parentToggled) {
            makeWritable(path.parent_path());
            *parentToggled = true;
        }
    } else {
        const auto dirOfPath = path.parent_path();
        if (dirOfPath != config->realStoreDir) {
            makeWritable(dirOfPath);
            /* When we're done, make the directory read-only again and
               reset its timestamp back to 0. */
            makeReadOnly.path = dirOfPath;
        }
    }

    /* makeTempPath would re-canonicalise the (constant) store dir with
       symlink resolution and run boost::format, once per linked file;
       build the name directly instead. */
    static std::atomic<uint32_t> tmpCounter(std::random_device{}());
    /* getpid() is a real syscall (glibc dropped the cache in 2.25) and
       the pid cannot change under us here, so resolve it once per
       process rather than once per linked file. */
    static const std::string pidPart = "/.tmp-link-" + std::to_string(getpid()) + "-";
    std::filesystem::path tempLink{
        config->realStoreDir.native() + pidPart
        + std::to_string(tmpCounter.fetch_add(1, std::memory_order_relaxed))};

    try {
        std::filesystem::create_hard_link(linkPath, tempLink);
        /* Note: do NOT insert st.st_ino here; that inode is being
           replaced. Marking it "linked" makes other files still on it
           skip the farm forever (dedup never converges). */
    } catch (std::filesystem::filesystem_error & e) {
        if (e.code() == std::errc::too_many_links) {
            /* Too many links to the same file (>= 32000 on most file
               systems).  This is likely to happen with empty files.
               Just shrug and ignore. */
            if (st.st_size)
                printInfo("%1% has maximum number of links", PathFmt(linkPath));
            return;
        }
        if (e.code() == std::errc::no_such_file_or_directory) {
            /* A concurrent garbage collection removed the link in the
               links directory. Skip optimising this path; a later pass
               will dedup it. */
            return;
        }
        throw SystemError(e.code(), "creating hard link from %1% to %2%", PathFmt(linkPath), PathFmt(tempLink));
    }

    /* Atomically replace the old file with the new hard link. */
    try {
        std::filesystem::rename(tempLink, path);
    } catch (std::filesystem::filesystem_error & e) {
        {
            std::error_code ec;
            remove(tempLink, ec); /* Clean up after ourselves. */
            if (ec)
                printError("unable to unlink %1%: %2%", PathFmt(tempLink), ec.message());
        }
        if (e.code() == std::errc::too_many_links) {
            /* Some filesystems generate too many links on the rename,
               rather than on the original link.  (Probably it
               temporarily increases the st_nlink field before
               decreasing it again.) */
            debug("%s has reached maximum number of links", PathFmt(linkPath));
            return;
        }
        throw SystemError(e.code(), "renaming %1% to %2%", PathFmt(tempLink), PathFmt(path));
    }

    ctx.filesLinked.fetch_add(1, std::memory_order_relaxed);
    /* Only regular files free data blocks. A symlink's st_size is its
       target length, and on ext4 a target under ~60 bytes lives inline
       in the inode, so collapsing two of them frees no blocks at all;
       counting st_size here reported savings that never existed. The
       link is still worth making (it shares the inode), it just is not
       bytes. */
    if (S_ISREG(st.st_mode))
        ctx.bytesFreed.fetch_add(st.st_size, std::memory_order_relaxed);

}

void LocalStore::optimiseStore(OptimiseStats & stats)
{
    std::lock_guard<std::mutex> runLock(optimiseStoreLock);

    auto paths = queryAllValidPaths();
    InodeHash inodeHash = loadInodeHash();

    /* Store paths are disjoint subtrees, so they can be deduplicated
       independently; the link farm races (create/unlink) are already
       handled for concurrent processes, which covers threads too.
       One task per path: the pool is already saturated at this
       granularity, so the per-path walks stay serial (no `pool`
       argument below) rather than fanning out a second time. */
    OptimiseCtx ctx(inodeHash);
    ThreadPool pool;

    for (auto & i : paths) {
        if (!isValidPath(i))
            continue; /* path was GC'ed, probably */
        pool.enqueue([&, i] {
            printTalkative("optimising path '%s'", printStorePath(i));
            optimisePath_(ctx, config->realStoreDir / i.to_string());
        });
    }

    pool.process();
    ctx.into(stats);
}

void LocalStore::optimisePath(const std::filesystem::path & path)
{
    InodeHash inodeHash;
    OptimiseCtx ctx(inodeHash);

    if (config->getLocalSettings().autoOptimiseStore)
        optimisePath_(ctx, path);
}

void LocalStore::optimisePath(const StorePath & path, OptimiseStats & stats, const ImportFileHashes * fileHashes)
{
    std::lock_guard<std::mutex> runLock(optimiseStoreLock);

    if (!isValidPath(path))
        return; /* path was GC'ed, probably */

    InodeHash inodeHash = loadInodeHash();
    std::filesystem::path realPath = config->realStoreDir / path.to_string();

    OptimiseCtx ctx(inodeHash);
    ctx.fileHashes = fileHashes;
    ctx.fileHashesBase = realPath.native().size();
    if (fileHashes) {
        /* `files` now holds symlinks as well as regular files, so it is
           the whole total; only the streamed-dedup swaps come off it,
           and those are regular files by construction (tryDedup is
           reached from End, which only regular files queue). Where
           symlinks cannot be farmed they are still walked and still
           counted, so the bar stays honest either way. */
        ctx.total = fileHashes->files.size() - fileHashes->dedupedFiles;
    }

    /* A commit optimises exactly one path, so the whole pass used to
       run on one thread while the box has all its others idle. The
       subtrees under this root are disjoint, so they fan out the same
       way whole-store paths do; the root's own file children stay on
       this thread, which is then the only writer of the root's
       writable-toggle. */
    ThreadPool pool;
    optimisePath_(ctx, realPath, nullptr, &pool);
    pool.process();
    ctx.into(stats);
}

} // namespace nix
