#include "nix/util/file-system.hh"
#include "nix/util/file-path.hh"
#include "nix/util/file-path-impl.hh"
#include "nix/util/signals.hh"
#include "nix/util/finally.hh"
#include "nix/util/serialise.hh"
#include "nix/util/util.hh"

#include <atomic>
#include <random>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include "nix/util/file-system-at.hh"
#include "nix/util/environment-variables.hh"
#include "util-config-private.hh"




namespace nix {

DirectoryIterator::DirectoryIterator(const std::filesystem::path & p)
{
    try {
        // **Attempt to create the underlying directory_iterator**
        it_ = std::filesystem::directory_iterator(p);
    } catch (const std::filesystem::filesystem_error & e) {
        // **Catch filesystem_error and throw SystemError**
        // Adapt the error message as needed for SystemError
        throw SystemError(e.code(), "cannot read directory %s", PathFmt(p));
    }
}

DirectoryIterator & DirectoryIterator::operator++()
{
    // **Attempt to increment the underlying iterator**
    std::error_code ec;
    it_.increment(ec);
    if (ec) {
        // Try to get path info if possible, might fail if iterator is bad
        try {
            if (it_ != std::filesystem::directory_iterator{}) {
                throw SysError("cannot read directory past %s: %s", PathFmt(it_->path()), ec.message());
            }
        } catch (...) {
            throw SysError("cannot read directory");
        }
    }
    return *this;
}

std::filesystem::path
absPath(const std::filesystem::path & path0, const std::filesystem::path * dir, bool resolveSymlinks)
{
    std::filesystem::path path = path0;

    if (!path.is_absolute()) {
        // In this case we need to call `canonPath` on a newly-created
        // string.
        if (!dir) {
            char buf[PATH_MAX];
            if (!getcwd(buf, sizeof(buf)))
                throw SysError("cannot get cwd");
            path = std::filesystem::path{buf} / path;
        } else
            path = *dir / path;
    }
    return canonPath(path, resolveSymlinks);
}

std::filesystem::path canonPath(const std::filesystem::path & path, bool resolveSymlinks)
{
    if (path.empty())
        throw Error("cannot canonicalise an empty path");

    if (!path.is_absolute())
        throw Error("not an absolute path: %s", PathFmt(path));

    /* This just exists because we cannot set the target of `remaining`
       (the callback parameter) directly to a newly-constructed string,
       since it is `std::string_view`. */
    std::string temp;

    /* Count the number of times we follow a symlink and stop at some
       arbitrary (but high) limit to prevent infinite loops. */
    unsigned int followCount = 0, maxFollow = 1024;

    auto ret = canonPathInner<OsPathTrait<char>>(
        path.string(),
        [&followCount, &temp, maxFollow, resolveSymlinks](std::string & result, std::string_view & remaining) {
            if (resolveSymlinks && std::filesystem::is_symlink(result)) {
                if (++followCount >= maxFollow)
                    throw Error("infinite symlink recursion in path '%1%'", remaining);
                remaining = (temp = concatStrings(readLink(result).string(), remaining));
                if (std::filesystem::path(remaining).is_absolute()) {
                    /* restart for symlinks pointing to absolute path */
                    result.clear();
                } else {
                    result = std::filesystem::path(result).parent_path().string();
                    if (result == "/") {
                        /* we don’t want trailing slashes here, which `dirOf`
                           only produces if `result = /` */
                        result.clear();
                    }
                }
            }
        });

    return ret;
}

std::string_view baseNameOf(std::string_view path)
{
    if (path.empty())
        return "";

    auto last = path.size() - 1;
    while (last > 0 && OsPathTrait<char>::isPathSep(path[last]))
        last -= 1;

    auto pos = OsPathTrait<char>::rfindPathSep(path, last);
    if (pos == path.npos)
        pos = 0;
    else
        pos += 1;

    return path.substr(pos, last - pos + 1);
}

bool isInDir(const std::filesystem::path & path, const std::filesystem::path & dir)
{
    /* Note that while the standard doesn't guarantee this, the
      `lexically_*` functions should do no IO and not throw. */
    auto rel = path.lexically_relative(dir);
    if (rel.empty())
        return false;

    auto first = *rel.begin();
    return first != "." && first != "..";
}

#  define STAT stat

PosixStat stat(const std::filesystem::path & path)
{
    PosixStat st;
    if (STAT(path.c_str(), &st))
        throw SysError("getting status of %s", PathFmt(path));
    return st;
}

std::optional<PosixStat> maybeStat(const std::filesystem::path & path)
{
    std::optional<PosixStat> st{std::in_place};
    if (STAT(path.c_str(), &*st)) {
        if (errno == ENOENT || errno == ENOTDIR)
            return std::nullopt;
        throw SysError("getting status of %s", PathFmt(path));
    }
    return st;
}

#undef STAT

bool pathExists(const std::filesystem::path & path)
{
    return maybeLstat(path).has_value();
}

std::filesystem::path readLink(const std::filesystem::path & path)
{
    checkInterrupt();
    try {
        return std::filesystem::read_symlink(path);
    } catch (std::filesystem::filesystem_error & e) {
        throw SystemError(e.code(), "reading symbolic link %s", PathFmt(path));
    }
}

std::string readFile(const std::filesystem::path & path)
{
    auto fd = openFileReadonly(path);
    if (!fd)
        throw NativeSysError("opening file %s", PathFmt(path));
    return readFile(fd.get());
}

void writeFile(
    const std::filesystem::path & path, std::string_view s, mode_t mode, FsSync sync, FinalSymlink finalSymlink)
{
    AutoCloseFD fd = openNewFileForWrite(
        path,
        mode,
        {
            .truncateExisting = true,
            .followSymlinksOnTruncate = (finalSymlink == FinalSymlink::Follow),
        });
    if (!fd)
        throw NativeSysError("opening file %s", PathFmt(path));

    writeFile(fd.get(), s, sync, &path);

    /* Close explicitly to propagate the exceptions. */
    fd.close();
}

void writeFile(Descriptor fd, std::string_view s, FsSync sync, const std::filesystem::path * origPath)
{
    assert(fd != INVALID_DESCRIPTOR);
    try {
        writeFull(fd, s);

        if (sync == FsSync::Yes)
            syncDescriptor(fd);

    } catch (Error & e) {
        e.addTrace("writing file %1%", origPath ? PathFmt(*origPath) : PathFmt(descriptorToPath(fd)));
        throw;
    }
}

void writeFile(const std::filesystem::path & path, Source & source, mode_t mode, FsSync sync, FinalSymlink finalSymlink)
{
    AutoCloseFD fd = openNewFileForWrite(
        path,
        mode,
        {
            .truncateExisting = true,
            .followSymlinksOnTruncate = (finalSymlink == FinalSymlink::Follow),
        });
    if (!fd)
        throw NativeSysError("opening file %s", PathFmt(path));

    std::array<char, 64 * 1024> buf;

    try {
        while (true) {
            try {
                auto n = source.read(buf.data(), buf.size());
                writeFull(fd.get(), {buf.data(), n});
            } catch (EndOfFile &) {
                break;
            }
        }
    } catch (Error & e) {
        e.addTrace("writing file %s", PathFmt(path));
        throw;
    }
    if (sync == FsSync::Yes)
        fd.fsync();
    // Explicitly close to make sure exceptions are propagated.
    fd.close();
    if (sync == FsSync::Yes)
        syncParent(path);
}

void syncParent(const std::filesystem::path & path)
{
    assert(path.has_parent_path());
    AutoCloseFD fd = openDirectory(path.parent_path(), FinalSymlink::Follow);
    if (!fd)
        throw NativeSysError("opening file %s", PathFmt(path));
    fd.fsync();
}

void recursiveSync(const std::filesystem::path & path)
{
    /* If it's a file or symlink, just fsync and return. */
    auto st = lstat(path);
    if (S_ISREG(st.st_mode)) {
        AutoCloseFD fd = openFileReadonly(path, FinalSymlink::DontFollow);
        if (!fd)
            throw NativeSysError("opening file %s", PathFmt(path));
        fd.fsync();
        return;
    } else if (S_ISLNK(st.st_mode))
        return;

    /* Otherwise, perform a depth-first traversal of the directory and
       fsync all the files. */
    std::deque<std::filesystem::path> dirsToEnumerate;
    dirsToEnumerate.push_back(path);
    std::vector<std::filesystem::path> dirsToFsync;
    while (!dirsToEnumerate.empty()) {
        auto currentDir = dirsToEnumerate.back();
        dirsToEnumerate.pop_back();
        for (auto & entry : DirectoryIterator(currentDir)) {
            auto st = entry.symlink_status();
            if (std::filesystem::is_directory(st)) {
                dirsToEnumerate.emplace_back(entry.path());
            } else if (std::filesystem::is_regular_file(st)) {
                AutoCloseFD fd = openFileReadonly(entry.path(), FinalSymlink::DontFollow);
                if (!fd)
                    throw NativeSysError("opening file %1%", PathFmt(entry.path()));
                fd.fsync();
            }
        }
        dirsToFsync.emplace_back(std::move(currentDir));
    }

    /* Fsync all the directories. */
    for (auto dir = dirsToFsync.rbegin(); dir != dirsToFsync.rend(); ++dir) {
        AutoCloseFD fd = openDirectory(*dir, FinalSymlink::DontFollow);
        if (!fd)
            throw NativeSysError("opening directory %1%", PathFmt(*dir));
        fd.fsync();
    }
}

void createDirs(const std::filesystem::path & path)
{
    try {
        std::filesystem::create_directories(path); // NOLINT(bugprone-unsafe-functions)
    } catch (std::filesystem::filesystem_error & e) {
        throw SystemError(e.code(), "creating directory %1%", PathFmt(path));
    }
}

//////////////////////////////////////////////////////////////////////

AutoDelete::AutoDelete()
    : del{false}
    , recursive(false)
{
}

AutoDelete::AutoDelete(const std::filesystem::path & p, bool recursive)
    : _path(p)
    , del(true)
    , recursive(recursive)
{
}

void AutoDelete::deletePath()
{
    if (del) {
        if (recursive)
            nix::deletePath(_path);
        else
            std::filesystem::remove(_path);
        cancel();
    }
}

AutoDelete::~AutoDelete()
{
    try {
        deletePath();
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

void AutoDelete::cancel() noexcept
{
    del = false;
}

std::filesystem::path createTempDir(const std::filesystem::path & tmpRoot, const std::string & prefix, mode_t mode)
{
    while (1) {
        checkInterrupt();
        std::filesystem::path tmpDir = makeTempPath(tmpRoot, prefix);
        if (mkdir(
                tmpDir.string().c_str()
                    ,
                mode
                )
            == 0) {
            return tmpDir;
        }
        if (errno != EEXIST)
            throw SysError("creating directory %1%", PathFmt(tmpDir));
    }
}

std::filesystem::path makeTempPath(const std::filesystem::path & root, const std::string & suffix)
{
    // start the counter at a random value to minimize issues with preexisting temp paths
    static std::atomic<uint32_t> counter(std::random_device{}());
    assert(!std::filesystem::path(suffix).is_absolute());
    auto tmpRoot = canonPath(root.empty() ? defaultTempDir() : root, true);
    return tmpRoot / fmt("%s-%s-%s", suffix, getpid(), counter.fetch_add(1, std::memory_order_relaxed));
}

void createSymlink(const std::filesystem::path & target, const std::filesystem::path & link)
{
    std::error_code ec;
    std::filesystem::create_symlink(target, link, ec);
    if (ec)
        throw SysError(ec.value(), "creating symlink %s -> %s", PathFmt(link), PathFmt(target));
}

void setWriteTime(const std::filesystem::path & path, const PosixStat & st)
{
    setWriteTime(path, st.st_atime, st.st_mtime);
}

void copyFile(const std::filesystem::path & from, const std::filesystem::path & to, bool andDelete, bool contents)
{
    auto fromStatus = std::filesystem::symlink_status(from);

    // Mark the directory as writable so that we can delete its children
    if (andDelete && std::filesystem::is_directory(fromStatus)) {
        std::filesystem::permissions(
            from,
            std::filesystem::perms::owner_write,
            std::filesystem::perm_options::add | std::filesystem::perm_options::nofollow);
    }

    if (std::filesystem::is_symlink(fromStatus) || std::filesystem::is_regular_file(fromStatus)) {
        if (contents) {
            std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing);
        } else {
            std::filesystem::copy(
                from,
                to,
                std::filesystem::copy_options::copy_symlinks | std::filesystem::copy_options::overwrite_existing);
        }
    } else if (std::filesystem::is_directory(fromStatus)) {
        std::filesystem::create_directory(to);
        for (auto & entry : DirectoryIterator(from)) {
            copyFile(entry, to / entry.path().filename(), andDelete);
        }
    } else {
        throw Error("file %s has an unsupported type", PathFmt(from));
    }

    setWriteTime(to, lstat(from));
    if (andDelete) {
        if (!std::filesystem::is_symlink(fromStatus))
            std::filesystem::permissions(
                from,
                std::filesystem::perms::owner_write,
                std::filesystem::perm_options::add | std::filesystem::perm_options::nofollow);
        std::filesystem::remove(from);
    }
}

void moveFile(const std::filesystem::path & oldName, const std::filesystem::path & newName)
{
    try {
        std::filesystem::rename(oldName, newName);
    } catch (std::filesystem::filesystem_error & e) {
        auto oldPath = oldName;
        auto newPath = newName;
        // For the move to be as atomic as possible, copy to a temporary
        // directory
        std::filesystem::path temp = createTempDir(os_string_to_string(PathView{newPath.parent_path()}), "rename-tmp");
        Finally removeTemp = [&]() { std::filesystem::remove(temp); };
        auto tempCopyTarget = temp / "copy-target";
        if (e.code().value() == EXDEV) {
            std::filesystem::remove(newPath);
            warn("can’t rename %s as %s, copying instead", PathFmt(oldName), PathFmt(newName));
            copyFile(oldPath, tempCopyTarget, true);
            std::filesystem::rename(
                os_string_to_string(PathView{tempCopyTarget}), os_string_to_string(PathView{newPath}));
        }
    }
}

//////////////////////////////////////////////////////////////////////

void chmod(const std::filesystem::path & path, mode_t mode)
{
    if (
        ::chmod
        (path.c_str(), mode)
        == -1)
        throw SysError("setting permissions on %s", PathFmt(path));
}

#  define UNLINK_PROC ::unlink

void unlinkIfExists(const std::filesystem::path & path)
{
    if (UNLINK_PROC(path.c_str()) == -1) {
        if (errno != ENOENT)
            throw SysError("removing %s", PathFmt(path));
    }
}

void unlink(const std::filesystem::path & path)
{
    if (UNLINK_PROC(path.c_str()) == -1)
        throw SysError("removing %s", PathFmt(path));
}

void tryUnlink(const std::filesystem::path & path)
{
    UNLINK_PROC(path.c_str());
}

#undef UNLINK_PROC

bool chmodIfNeeded(const std::filesystem::path & path, mode_t mode, mode_t mask)
{
    auto prevMode = lstat(path).st_mode;

    if (((prevMode ^ mode) & mask) == 0)
        return false;

    chmod(path, mode);
    return true;
}

AutoCloseFD openDirectory(const std::filesystem::path & path, FinalSymlink finalSymlink)
{
    return AutoCloseFD{open(
        path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | (finalSymlink == FinalSymlink::Follow ? 0 : O_NOFOLLOW))};
}

AutoCloseFD openFileReadonly(const std::filesystem::path & path, FinalSymlink finalSymlink)
{
    return AutoCloseFD{
        open(path.c_str(), O_RDONLY | O_CLOEXEC | (finalSymlink == FinalSymlink::Follow ? 0 : O_NOFOLLOW))};
}

AutoCloseFD openNewFileForWrite(const std::filesystem::path & path, mode_t mode, OpenNewFileForWriteParams params)
{
    auto flags = (params.writeOnly ? O_WRONLY : O_RDWR) | O_CREAT | O_CLOEXEC;
    if (params.truncateExisting) {
        flags |= O_TRUNC;
        if (!params.followSymlinksOnTruncate)
            flags |= O_NOFOLLOW;
    } else {
        flags |= O_EXCL; /* O_CREAT | O_EXCL already ensures that symlinks are not followed. */
    }
    return AutoCloseFD{open(path.c_str(), flags, mode)};
}

std::filesystem::path descriptorToPath(Descriptor fd)
{
    if (fd == STDIN_FILENO)
        return "<stdin>";
    if (fd == STDOUT_FILENO)
        return "<stdout>";
    if (fd == STDERR_FILENO)
        return "<stderr>";

    try {
        return readLink("/proc/self/fd/" + std::to_string(fd));
    } catch (SystemError &) {
    }

    /* Fallback for unknown fd or unsupported platform */
    return "<fd " + std::to_string(fd) + ">";
}

std::filesystem::path defaultTempDir()
{
    return getEnvOsNonEmpty("TMPDIR").value_or("/tmp");
}

PosixStat lstat(const std::filesystem::path & path)
{
    PosixStat st;
    if (::lstat(path.c_str(), &st))
        throw SysError("getting status of %s", PathFmt(path));
    return st;
}

std::optional<PosixStat> maybeLstat(const std::filesystem::path & path)
{
    std::optional<PosixStat> st{std::in_place};
    if (::lstat(path.c_str(), &*st)) {
        if (errno == ENOENT || errno == ENOTDIR)
            return std::nullopt;
        throw SysError("getting status of %s", PathFmt(path));
    }
    return st;
}

void setWriteTime(const std::filesystem::path & path, time_t accessedTime, time_t modificationTime)
{
    // Would be nice to use std::filesystem unconditionally, but
    // doesn't support access time just modification time.
    //
    // System clock vs File clock issues also make that annoying.
    struct timespec times[2] = {
        {
            .tv_sec = accessedTime,
            .tv_nsec = 0,
        },
        {
            .tv_sec = modificationTime,
            .tv_nsec = 0,
        },
    };
    if (utimensat(AT_FDCWD, path.c_str(), times, AT_SYMLINK_NOFOLLOW) == -1)
        throw SysError("changing modification time of %s (using `utimensat`)", PathFmt(path));
}

#  define MOUNTEDPATHS_PARAM
#  define MOUNTEDPATHS_ARG

static void _deletePath(
    Descriptor parentfd,
    const std::filesystem::path & path,
    uint64_t & bytesFreed,
    std::exception_ptr & ex MOUNTEDPATHS_PARAM)
{
    checkInterrupt();

    auto name = CanonPath::fromFilename(path.filename().native());

    auto st_ = maybeFstatat(parentfd, name.rel());
    if (!st_)
        return;
    auto & st = *st_;

    if (!S_ISDIR(st.st_mode)) {
        /* We are about to delete a file. Will it likely free space? */

        switch (st.st_nlink) {
        /* Yes: last link. */
        case 1:
            bytesFreed += st.st_size;
            break;
        /* Maybe: yes, if 'auto-optimise-store' or manual optimisation
           was performed. Instead of checking for real let's assume
           it's an optimised file and space will be freed.

           In worst case we will double count on freed space for files
           with exactly two hardlinks for unoptimised packages.
         */
        case 2:
            bytesFreed += st.st_size;
            break;
        /* No: 3+ links. */
        default:
            break;
        }
    }

    if (S_ISDIR(st.st_mode)) {
        /* Make the directory accessible. */
        const auto PERM_MASK = S_IRUSR | S_IWUSR | S_IXUSR;
        if ((st.st_mode & PERM_MASK) != PERM_MASK)
            try {
                fchmodatTryNoFollow(parentfd, name, st.st_mode | PERM_MASK);
            } catch (SysError & e) {
                e.addTrace("while making directory %1% accessible for deletion", PathFmt(path));
                if (e.errNo == EOPNOTSUPP)
                    e.addTrace("%1% is now a symlink, expected directory", PathFmt(path));
                throw;
            }

        int fd = openat(parentfd, name.rel_c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd == -1)
            throw SysError("opening directory %1%", PathFmt(path));
        AutoCloseDir dir(fdopendir(fd));
        if (!dir)
            throw SysError("opening directory %1%", PathFmt(path));

        struct dirent * dirent;
        while (errno = 0, dirent = readdir(dir.get())) { /* sic */
            checkInterrupt();
            std::string childName = dirent->d_name;
            if (childName == "." || childName == "..")
                continue;
            _deletePath(dirfd(dir.get()), path / childName, bytesFreed, ex MOUNTEDPATHS_ARG);
        }
        if (errno)
            throw SysError("reading directory %1%", PathFmt(path));
    }

    int flags = S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0;
    if (unlinkat(parentfd, name.rel_c_str(), flags) == -1) {
        if (errno == ENOENT)
            return;
        try {
            throw SysError("cannot unlink %1%", PathFmt(path));
        } catch (...) {
            if (!ex)
                ex = std::current_exception();
            else
                ignoreExceptionExceptInterrupt();
        }
    }
}

static void _deletePath(const std::filesystem::path & path, uint64_t & bytesFreed MOUNTEDPATHS_PARAM)
{
    assert(path.is_absolute());
    auto parentDirPath = path.parent_path();
    assert(parentDirPath != path);

    AutoCloseFD dirfd = openDirectory(parentDirPath);
    if (!dirfd) {
        if (errno == ENOENT)
            return;
        throw SysError("opening directory %s", PathFmt(parentDirPath));
    }

    std::exception_ptr ex;

    _deletePath(dirfd.get(), path, bytesFreed, ex MOUNTEDPATHS_ARG);

    if (ex)
        std::rethrow_exception(ex);
}

void deletePath(const std::filesystem::path & path)
{
    uint64_t dummy;
    deletePath(path, dummy);
}

void deletePath(const std::filesystem::path & path, uint64_t & bytesFreed)
{
    // Activity act(*logger, lvlDebug, "recursively deleting path '%1%'", path);
    bytesFreed = 0;
    _deletePath(path, bytesFreed MOUNTEDPATHS_ARG);
}

void chown(const std::filesystem::path & path, uid_t owner, gid_t group)
{
    if (::chown(path.c_str(), owner, group) == -1)
        throw SysError("changing ownership of %s", PathFmt(path));
}

} // namespace nix
