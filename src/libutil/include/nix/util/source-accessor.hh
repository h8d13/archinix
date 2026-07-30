#pragma once

#include <filesystem>

#include "nix/util/canon-path.hh"
#include "nix/util/fun.hh"
#include "nix/util/hash.hh"
#include "nix/util/ref.hh"

namespace nix {

struct Sink;

MakeError(SourceAccessorError, Error);
MakeError(FileNotFound, SourceAccessorError);
MakeError(NotASymlink, SourceAccessorError);
MakeError(NotADirectory, SourceAccessorError);
MakeError(NotARegularFile, SourceAccessorError);

/**
 * A read-only filesystem abstraction. This is used by the Nix
 * evaluator and elsewhere for accessing sources in various
 * filesystem-like entities (such as the real filesystem, tarballs or
 * Git repositories).
 */
struct SourceAccessor : std::enable_shared_from_this<SourceAccessor>
{
    const size_t number;

    std::string displayPrefix, displaySuffix;

    SourceAccessor();

    virtual ~SourceAccessor() {}

    /**
     * Write the contents of a file as a sink. `sizeCallback` must be
     * called with the size of the file before any data is written to
     * the sink.
     *
     * @note This method should *not* follow symlinks.
     */
    virtual void
    readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback = [](uint64_t size) {}) = 0;

    enum Type {
        tRegular,
        tSymlink,
        tDirectory,
        /**
          Any other node types that may be encountered on the file system, such as device nodes, sockets, named pipe,
          and possibly even more exotic things.

          NAR cannot represent these, so dumpPath skips sockets and
          fifos and throws on the rest.

          Unlike `DT_UNKNOWN`, this must not be used for deferring the lookup of types.
        */
        tChar,
        tBlock,
        tSocket,
        tFifo,
        tUnknown
    };

    struct Stat
    {
        Type type = tUnknown;

        /**
         * For regular files only: the size of the file. Not all
         * accessors return this since it may be too expensive to
         * compute.
         */
        std::optional<uint64_t> fileSize;

        /**
         * For regular files only: whether this is an executable.
         */
        bool isExecutable = false;

        /**
         * For regular files only: the position of the contents of this
         * file in the NAR. Only returned by NAR accessors.
         */
        std::optional<uint64_t> narOffset;

    };

    Stat lstat(const CanonPath & path);

    virtual std::optional<Stat> maybeLstat(const CanonPath & path) = 0;

    typedef std::optional<Type> DirEntry;

    typedef std::map<std::string, DirEntry> DirEntries;

    /**
     * @note Like `readFile`, this method should *not* follow symlinks.
     */
    virtual DirEntries readDirectory(const CanonPath & path) = 0;

    /**
     * Variation of readDirectory that receives a SourceAccessor possibly scoped to \ref dirPath,
     * together with that directory's entries. Primary meant for recursive traversal functions that
     * would benefit from *at-style syscalls relative to a particular directory.
     *
     * The entries come with the accessor because a traversal needs both:
     * reading them separately opens the directory a second time, and
     * then the names come from one directory instance while the descent
     * happens in another.
     *
     * @note Like `readFile`, this method should *not* follow symlinks.
     * @param callback Caller-provided function invoked with a maximally deeply scoped SourceAccessor, the path that
     * would have to be prepended to each path relative to dirPath to access a particular file with it, and the
     * entries of dirPath.
     */
    virtual void readDirectory(
        const CanonPath & dirPath,
        const std::function<void(SourceAccessor & subdirAccessor, const CanonPath & subdirRelPath, DirEntries entries)>
            & callback)
    {
        callback(*this, dirPath, readDirectory(dirPath));
    }

    virtual std::string readLink(const CanonPath & path) = 0;

    void dumpPath(const CanonPath & path, Sink & sink);

    bool operator==(const SourceAccessor & x) const
    {
        return number == x.number;
    }

    auto operator<=>(const SourceAccessor & x) const
    {
        return number <=> x.number;
    }

    void setPathDisplay(std::string displayPrefix, std::string displaySuffix = "");

    virtual std::string showPath(const CanonPath & path);

};

class SymlinkNotAllowed final : public Error
{
    void anchor() override;

public:
    CanonPath path;

    SymlinkNotAllowed(CanonPath path)
        : Error("relative path '%s' points to a symlink, which is not allowed", path.rel())
        , path(std::move(path))
    {
    }

    template<typename... Args>
    SymlinkNotAllowed(CanonPath path, const std::string & fs, Args &&... args)
        : Error(fs, std::forward<Args>(args)...)
        , path(std::move(path))
    {
    }
};

/**
 * Construct an accessor for the filesystem rooted at `root`. Note
 * that it is not possible to escape `root` by appending `..` path
 * elements, and that absolute symlinks are resolved relative to
 * `root`.
 *
 * Symlinks in parents of `root` are resolved. Final symlink is not.
 */
ref<SourceAccessor> makeFSSourceAccessor(std::filesystem::path root);

} // namespace nix
