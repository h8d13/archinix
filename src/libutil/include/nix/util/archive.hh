#pragma once
///@file

#include "nix/util/types.hh"
#include "nix/util/serialise.hh"
#include "nix/util/fs-sink.hh"
#include "nix/util/hash.hh"

namespace nix {

struct SourcePath;

/**
 * dumpPath creates a Nix archive of the specified path.
 *
 * @param path the file system data to dump. Dumping is recursive so if
 * this is a directory we dump it and all its children.
 *
 * @param [out] sink The serialised archive is fed into this sink.
 *
 * @param filter Can be used to skip certain files.
 *
 * The format is as follows:
 *
 * ```
 * IF path points to a REGULAR FILE:
 *   dump(path) = attrs(
 *     [ ("type", "regular")
 *     , ("contents", contents(path))
 *     ])
 *
 * IF path points to a DIRECTORY:
 *   dump(path) = attrs(
 *     [ ("type", "directory")
 *     , ("entries", concat(map(f, sort(entries(path)))))
 *     ])
 *     where f(fn) = attrs(
 *       [ ("name", fn)
 *       , ("file", dump(path + "/" + fn))
 *       ])
 *
 * where:
 *
 *   attrs(as) = concat(map(attr, as)) + encN(0)
 *   attrs((a, b)) = encS(a) + encS(b)
 *
 *   encS(s) = encN(len(s)) + s + (padding until next 64-bit boundary)
 *
 *   encN(n) = 64-bit little-endian encoding of n.
 *
 *   contents(path) = the contents of a regular file.
 *
 *   sort(strings) = lexicographic sort by 8-bit value (strcmp).
 *
 *   entries(path) = the entries of a directory, without `.` and
 *   `..`.
 *
 *   `+` denotes string concatenation.
 * ```
 */
void dumpPath(const std::filesystem::path & path, Sink & sink);

void parseDump(FileSystemObjectSink & sink, Source & source);

/**
 * Hash of the NAR of `path`, plus the number of bytes hashed.
 *
 * The NAR is the only serialisation this fork has (flat-file ingestion
 * went with the fixed-output derivations that needed it), so the hash
 * of a path means this and nothing else.
 */
HashResult hashPath(const SourcePath & path);

/* canonical: restore with store-canonical metadata baked in (see
   RestoreSink::canonical), making a post-restore canonicalise walk
   redundant */
void restorePath(const std::filesystem::path & path, Source & source, bool startFsync = false, bool canonical = false);

inline constexpr std::string_view narVersionMagic1 = "nix-archive-1";

} // namespace nix
