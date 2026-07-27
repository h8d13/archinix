# libutil + libstore: the store layer API

Two shared libraries, ~20k lines, no Nix on the host, no daemon, no
network:

| lib | pkg-config | headers | what it holds |
|---|---|---|---|
| `libnixutil` | `nix-util` | `include/nix/util/` | NAR archive, hashing, source accessors, serialisation, fs helpers |
| `libnixstore` | `nix-store` | `include/nix/store/` | store dir + sqlite db, GC, dedup farm, export |

## One layer, two libraries

The split is by subject, not by optionality. libutil knows bytes, files
and hashes and has no notion of a store; libstore adds the store dir,
the db and the GC on top. Content addressing is the seam: libutil
serialises a tree to a NAR and hashes it, libstore turns that hash into
a name and a registration.

You link both whether you plan to or not:

```
libnixstore.so -> libnixutil.so, libsqlite3.so
libnixutil.so  -> libblake3.so, libcrypto.so, libboost_context.so
```

`nix-store.pc` carries `Requires: nix-util`, so `pkg-config --cflags
--libs nix-store` already emits `-lnixstore -lnixutil`. Naming both is
just explicit.

And the store API is written in libutil's types, so its headers are in
your translation unit regardless:

| you call | you need |
|---|---|
| anything | `ref<Store>` (`util/ref.hh`), `Error`/`SysError` (`util/error.hh`) |
| `addToStoreFromDump`, `exportPaths` | `Source`/`Sink` (`util/serialise.hh`) |
| producing a NAR from disk | `SourceAccessor`, `SourcePath`, `CanonPath` (`util/source-accessor.hh`, `util/canon-path.hh`) |
| reading one back | `parseDump`, `FileSystemObjectSink` (`util/archive.hh`, `util/fs-sink.hh`) |
| `ValidPathInfo`, CA helpers | `Hash`, `HashAlgorithm` (`util/hash.hh`) |
| quieting the library | `verbosity` (`util/logging.hh`) |

## What is left

One store type remains, the local one.

So this store never *produces* a path. It ingests a directory tree you
built elsewhere and gives it a content-addressed name, a sqlite
registration, GC roots and file-level sharing with every other path.

It mounts nothing: an object store for trees, plus a garbage collector.

## On-disk layout

Everything derives from one absolute root path:

```
<root>/nix/store/<32-char-hash>-<name>/   the tree, read-only, all mtimes 1
<root>/nix/store/.links/<hash>            dedup farm: one inode per unique file
<root>/nix/var/nix/db/db.sqlite           ValidPaths + Refs (source of truth)
<root>/nix/var/nix/db/reserved            8 MiB so GC can run on a full disk
<root>/nix/var/nix/gcroots/<any-name>     symlink = its target stays alive
<root>/nix/var/nix/profiles/              also scanned as roots
```

Hash is base-32 (160 bits) over the SHA-256 of the path's NAR
serialisation; `StorePath::HashLen == 32`, whole basename capped at 211
chars.

Schema is 30 lines, in [`libstore/schema.sql`](libstore/schema.sql):
`ValidPaths(path, hash, registrationTime, narSize, ca, ...)` and
`Refs(referrer, reference)`.

Two consequences before you write against this:

- **A directory listing is not the store.** `.links` sits in it
  permanently, and nothing stops two valid paths ending in the same
  `-name`, so a glob is ambiguous by design. (`tmp-*` staging dirs are
  also visible, but only while an import runs or after one was killed;
  the next GC sweep takes them.) The db says what is valid.
- **mtimes are canonicalised to 1** on import, so "newest" is
  meaningless on disk. `registrationTime` from the db is the only
  "added at" a store path has.

## Build and link

```sh
./build.sh                          # PREFIX=$PWD/build/prefix by default
PREFIX=/usr/local ./build.sh        # or install system-wide
```

Stage order matters: `libutil` installs first, `libstore` resolves it
through `pkg-config`. Any consumer is then one line:

```sh
g++ -std=c++23 -O2 mytool.cc -o mytool \
	$(pkg-config --cflags --libs nix-store nix-util)
```

With the default prefix, add `PKG_CONFIG_PATH=$PWD/build/prefix/lib/pkgconfig`
to compile and `LD_LIBRARY_PATH=$PWD/build/prefix/lib` to run.

The [`Dockerfile`](../Dockerfile) is the same build on Debian, headers and
libs installed, as illustrative example to build anywhere.

## API, in call order

Signatures abbreviated; headers carry the full ones.

### Open

```c++
#include <nix/store/globals.hh>     // initLibStore(), verbosity
#include <nix/store/store-open.hh>

ref<Store> openStore(const std::filesystem::path & root,
                     bool mustExist = false);
```

A path and nothing else: no URI parsing, no settings channel, no env
vars. Knobs live in [`local-settings.hh`](libstore/include/nix/store/local-settings.hh)
as plain defaults you edit and recompile (`fsyncMetadata`,
`useSQLiteWAL`, `reservedSize`, ...).

Opening **creates the whole skeleton** if missing, so a typo or an
unattached mountpoint would otherwise read as an empty store and
succeed.

Readers pass `mustExist = true`; writers leave it false so a
blank disk gets its store on first import.

Downcast for the concrete surfaces:

```c++
auto local = store.dynamic_pointer_cast<LocalStore>();  // local-store.hh
auto & gc  = require<GcStore>(*store);                  // store-cast.hh
auto & fs  = require<LocalFSStore>(*store);
```

### Write

```c++
// local-store.hh: stream a NAR in, get a content-addressed path back
StorePath addToStoreFromDump(Source & dump, std::string_view name,
	FileSerialisationMethod, ContentAddressMethod, HashAlgorithm,
	const StorePathSet & references, RepairFlag repair,
	ImportFileHashes * fileHashes = nullptr);

// store-api.hh: register a path whose info you already have
void addToStore(const ValidPathInfo & info, Source & narSource,
	RepairFlag repair = NoRepair);
```

Produce the NAR with `SourcePath{...}.dumpPath(sink)`
(`util/source-accessor.hh`). Sockets and fifos are skipped by the dumper
itself (`archive.cc`): NAR has no representation for them, so they are
absent from the imported path rather than an error.

Import strips ACLs, canonicalises mtimes to 1, makes the result read-only.

`ImportFileHashes` is an out-param: per-file hashes captured while the
NAR streams, handed to `optimisePath` so dedup does not re-read what was
just written.

### Query

```c++
bool          isValidPath(const StorePath &);
StorePathSet  queryAllValidPaths();
StorePathSet  queryPathsByHashPrefix(const std::string &);   // LocalStore
ref<const ValidPathInfo> queryPathInfo(const StorePath &);
void          queryReferrers(const StorePath &, StorePathSet & out);
```

`ValidPathInfo` (`path-info.hh`) carries `narHash`, `narSize`,
`registrationTime`, `references`, `deriver`, `ca`.
`queryPathsByHashPrefix` matches a hash-part prefix of any length
through the db, so partial ids resolve without a directory glob.

### Roots

```c++
// local-fs-store.hh
std::filesystem::path addPermRoot(const StorePath &,
                                  const std::filesystem::path & gcRoot);
```

A symlink under `<root>/nix/var/nix/gcroots/`. The historical
`gcroots/auto` indirection is gone: roots live directly in the scanned
dir. Anything not reachable from a root is garbage.

### Dedup

```c++
// local-store.hh
void optimisePath(const StorePath &, OptimiseStats &,
                  const ImportFileHashes * = nullptr);
void optimiseStore(OptimiseStats &);
```

Hash each regular file, hardlink it to `.links/<hash>` when the content
is already there. `OptimiseStats` reports `filesLinked` and
`bytesFreed`. `optimisePath` does one path, `optimiseStore` walks every
valid path; passing the `ImportFileHashes` from the import skips
re-hashing what was just written.

Sharing is per file, below the store-path level, so two paths that
differ in a few files hold one inode per distinct content between them.

### Delete

```c++
#include <nix/store/gc-store.hh>

GCOptions opts;
opts.action = GCOptions::gcDeleteSpecific;      // or gcDeleteDead,
opts.pathsToDelete = GCOptions::SpecificPaths{  //    gcReturnLive/Dead
	.paths = {path}, .deleteReferrers = false};

GCResults results;
gcStore.collectGarbage(opts, results);          // results.bytesFreed
```

Disk and db together, refusing anything still rooted or referenced, and
pruning the farm. Drop the gcroot symlink first if you mean it. Never
`rm -rf` inside a store: the tree goes, the registration stays, and
`verifyStore` is where you find out.

### Ship

```c++
#include <nix/store/export-import.hh>
void exportPaths(Store &, const StorePathSet &, Sink &);
```

`nix-store --export` wire format, topologically sorted, re-hashed
against the db on the way out so local corruption cannot spread.

There is no `importPaths` counterpart: it went with signing, and
upstream's version trusted the path name the stream claimed. Receiving
is a loop you write over `parseDump` and `addToStore`. With signatures
gone, the identity check available to you is content addressing:
re-hash the received NAR, recompute the path with
`makeFixedOutputPathFromCA` (`store-dir-config.hh`), reject a mismatch.

### Verify

```c++
bool verifyStore(bool checkContents, RepairFlag repair);   // true == problems
```

Default level checks registrations are consistent (db paths exist,
referrers resolve). `checkContents` re-hashes every store path and every
`.links` entry against the recorded NAR hash: slow, and the one that
catches bitrot.

Detect only: `verifyStore` refuses a `Repair` flag with `Unsupported`,
since repairing meant re-fetching from a substituter. A missing or corrupt path is reported, not pruned; delete
it deliberately through the GC.

### Paths on disk

```c++
std::filesystem::path toRealPath(const StorePath &);   // <root>/nix/store/<base>
local->config->realStoreDir;                           // <root>/nix/store
local->config->stateDir;                               // <root>/nix/var/nix
```

Callers pass a store root and never spell out `nix/store` or
`nix/var/nix/db` themselves. Layout stays in the library.

## Minimal program

Import a directory, list the store, print where the new path lives:

```c++
#include <cstdio>
#include <filesystem>

#include <nix/store/globals.hh>
#include <nix/store/local-store.hh>
#include <nix/store/store-open.hh>
#include <nix/util/archive.hh>
#include <nix/util/serialise.hh>
#include <nix/util/source-accessor.hh>

using namespace nix;

int main(int argc, char ** argv)
try {
	initLibStore();                 // once, before anything else
	verbosity = lvlError;           // library logs to stderr otherwise

	auto store = openStore(std::filesystem::absolute(argv[1]));
	auto local = store.dynamic_pointer_cast<LocalStore>();

	std::optional<StorePath> imported;
	auto sink = sourceToSink([&](Source & source) {
		imported = local->addToStoreFromDump(source, "mytree",
			FileSerialisationMethod::NixArchive,
			ContentAddressMethod::Raw::NixArchive,
			HashAlgorithm::SHA256, {}, NoRepair);
	});
	SourcePath{makeFSSourceAccessor(std::filesystem::absolute(argv[2])),
		CanonPath::root}.dumpPath(*sink);
	sink->finish();

	for (auto & p : local->queryAllValidPaths())
		printf("%s\t%lld\n", std::string(p.to_string()).c_str(),
			(long long) local->queryPathInfo(p)->registrationTime);

	printf("%s\n", local->toRealPath(*imported).c_str());
	return 0;
} catch (std::exception & e) {
	fprintf(stderr, "%s\n", e.what());   // nix::Error derives from it
	return 1;
}
```

## Gotchas

- `initLibStore()` before anything, exactly once. `verbosity = lvlError`
  unless you want the library's progress on stderr.
- Every failure is an exception (`nix::Error`, `nix::SysError`,
  `nix::BadStorePath`, all `std::exception`). Catch at `main`: an ENOSPC
  mid-import must not reach `std::terminate`, or the partial temp dir
  never unwinds.
- `openStore` needs an **absolute** path.
- Concurrent importers are fine: sqlite runs in WAL mode, per-path
  locks cover the rest. Opening takes `db/big-lock` shared (it only
  blocks schema upgrades); GC takes `gc.lock` exclusive, so a sweep and
  an import serialise against each other.
- Store paths are read-only; `chmod -R u+w` before deleting a test root
  by hand.
