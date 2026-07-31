#pragma once
///@file

#include "nix/util/serialise.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/file-system.hh"

namespace nix {

/**
 * Actions on an open regular file in the process of creating it.
 *
 * See `FileSystemObjectSink::createRegularFile`.
 */
struct CreateRegularFileSink : Sink
{
private:
    void anchor() override;

public:
    /**
     * If set to true, the sink will not be called with the contents
     * of the file. `preallocateContents()` will still be called to
     * convey the file size. Useful for sinks that want to efficiently
     * discard the contents of the file.
     */
    bool skipContents = false;

    virtual void isExecutable() = 0;

    /**
     * An optimization. By default, do nothing.
     */
    virtual void preallocateContents(uint64_t size) {};
};

struct FileSystemObjectSink
{
private:
    /* Key function: gives the vtable one definition site instead of a
       weak copy in every TU. */
    virtual void anchor();

public:
    virtual ~FileSystemObjectSink() = default;

    virtual void createDirectory(const CanonPath & path) = 0;

    using DirectoryCreatedCallback = fun<void(FileSystemObjectSink & dirSink, const CanonPath & dirRelPath)>;

    /**
     * Create a directory and invoke a callback with a pair of sink + CanonPath
     * of the created subdirectory relative to dirSink.
     *
     * @note This allows for UNIX RestoreSink implementations to implement
     * *at-style accessors that always keep an open file descriptor for the
     * freshly created directory. Use this when it's important to disallow any
     * intermediate path components from being symlinks.
     */
    virtual void createDirectory(const CanonPath & path, const DirectoryCreatedCallback & callback)
    {
        createDirectory(path);
        callback(*this, path);
    }

    /**
     * This function in general is no re-entrant. Only one file can be
     * written at a time.
     */
    virtual void createRegularFile(const CanonPath & path, fun<void(CreateRegularFileSink &)>) = 0;

    virtual void createSymlink(const CanonPath & path, const std::string & target) = 0;
};

/**
 * Ignore everything and do nothing
 */
struct NullFileSystemObjectSink : FileSystemObjectSink
{
private:
    void anchor() override;

public:
    using FileSystemObjectSink::createDirectory; /* keep the callback overload visible */

    void createDirectory(const CanonPath & path) override {}

    void createSymlink(const CanonPath & path, const std::string & target) override {}

    void createRegularFile(const CanonPath & path, fun<void(CreateRegularFileSink &)>) override;
};

/**
 * Write files at the given path
 */
struct RestoreSink final : FileSystemObjectSink
{
private:
    void anchor() override;

public:
    std::filesystem::path dstPath;
    /**
     * File descriptor for the directory located at dstPath. Used for *at
     * operations relative to this file descriptor. This sink must *never*
     * follow intermediate symlinks (starting from dstPath) in case a file
     * collision is encountered for various reasons like case-insensitivity or
     * other types on normalization. using appropriate *at system calls and traversing
     * only one path component at a time ensures that writing is race-free and is
     * is not susceptible to symlink replacement.
     */
    AutoCloseFD dirFd;
    bool startFsync = false;

    /**
     * Restore with store-canonical metadata as each node completes:
     * files 0444/0555 at creation, mtime 1 via the still-open fd,
     * directories fchmod+futimens when their subtree is done, symlink
     * mtimes set through the parent fd. A NAR restore creates every
     * node itself, so the post-restore canonicalisePathMetaData walk
     * (lstat + llistxattr + chmod + utimensat per entry, and a second
     * journaled metadata write per file on the store medium) becomes
     * redundant and the caller can skip it. Callers must finish the
     * root directory themselves (finishCanonical()): it is the one
     * node still open when parsing ends.
     */
    bool canonical = false;

    /**
     * When set (canonical mode only), completed directories are
     * recorded here instead of being touched, children before
     * parents. For callers whose streamed dedup renames files
     * asynchronously: a rename would bump the just-stamped mtime of
     * the containing directory, so directory metadata must land after
     * that machinery drains.
     *
     * Paths are relative to this sink's root, so the caller can do the
     * deferred work through the root descriptor (which it still holds)
     * rather than by absolute path from AT_FDCWD: same two syscalls per
     * directory, but they cannot be redirected by a symlink planted
     * under the tree being restored.
     */
    std::vector<CanonPath> * deferCanonicalDirs = nullptr;

    /**
     * This sink's directory relative to the root of the restore, which
     * is what `deferCanonicalDirs` records.
     */
    CanonPath relToRoot = CanonPath::root;

    /**
     * The write buffer of the file currently being restored, parked
     * here between files. Every regular file gets its own sink over
     * its own descriptor, and each of those would otherwise malloc a
     * fresh 32 KiB buffer: on an import that is one malloc/free pair
     * per file, and the buffer is oversized for most of them. A NAR is
     * depth-first, so at most one file is open at a time and one
     * buffer serves the whole restore; it is handed down to the
     * subdirectory sinks and taken back when their subtree closes.
     */
    std::unique_ptr<char[]> fileBuf;

    explicit RestoreSink(bool startFsync, bool canonical = false)
        : startFsync{startFsync}
        , canonical{canonical}
    {
    }

    void createDirectory(const CanonPath & path) override;

    void createDirectory(const CanonPath & path, const DirectoryCreatedCallback & callback) override;

    void createRegularFile(const CanonPath & path, fun<void(CreateRegularFileSink &)>) override;

    void createSymlink(const CanonPath & path, const std::string & target) override;

    /**
     * Canonicalise the root directory (the node whose fd this sink
     * holds); no-op for non-canonical sinks and non-directory roots.
     */
    void finishCanonical();
};

} // namespace nix
