// Import any directory tree into a local store as one content-addressed
// store path, then hard-link-deduplicate it against the store's link
// farm. Only the new path is optimised (older paths are already
// farm-linked), using per-file hashes captured while the import
// streamed through, so nothing is read twice.
// stdout is exactly the imported store path (store mtimes are all
// canonicalised to 1, so callers must capture it instead of picking
// the "newest" by mtime); diagnostics go to stderr.
// Sockets and fifos are skipped during the dump (NAR cannot represent
// them); callers no longer need to delete them from live trees first.
// The imported path is registered as a GC root under
// <root>/nix/var/nix/gcroots/<basename>, so the store db itself knows
// generations are alive; rm-path drops the root before deleting.
// The import's writer thread runs at real-time round-robin priority
// (see src/libutil/scheduling.cc for which thread and why): the box
// grants the capability for it in arch/iso/setup-boot.sh, and where it
// is not granted the thread carries on at normal priority.
// usage: import-dir <store-root> <name> <dir>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <sys/stat.h>

#include <nix/store/globals.hh>
#include <nix/store/local-store.hh>
#include <nix/store/store-open.hh>
#include <nix/util/archive.hh>
#include <nix/util/progress.hh>
#include <nix/util/scheduling.hh>
#include <nix/util/serialise.hh>
#include <nix/util/source-accessor.hh>

using namespace nix;

int main(int argc, char ** argv)
try {
	if (argc != 4) {
		fprintf(stderr, "usage: %s <store-root> <name> <dir>\n", argv[0]);
		return 1;
	}

	initLibStore();
	/* lvlWarn, not lvlError: a refused real-time priority and the
	   dedup's unlink complaint are warnings, and a commit that
	   silently ran at normal priority is indistinguishable from one
	   that could not. Everything below lvlWarn (the import's own
	   progress, notices) stays quiet, which is what this was for. */
	verbosity = lvlWarn;

	/* read before the import: the writer thread applies it at start */
	realtimeWriter = true;

	auto store = openStore(std::filesystem::absolute(argv[1]));
	auto dir = std::filesystem::absolute(argv[3]);

	/* imported trees carry secrets (shadow, ssh host keys) at
	   canonical 0444, and restmeta re-protects only the booted
	   overlay view: the store dir inode itself (with the .links
	   farm inside it) is what /nixstore bind-mounts, so its mode is
	   the one path-credential gate. Everything legitimate reads the
	   store as root or offline (GRUB), so close it to everyone
	   else. Runs on every import: heals stores created before this
	   gate existed. */
	if (::chmod(store->config->realStoreDir.c_str(), 0700) == -1) {
		fprintf(stderr, "import-dir: chmod 0700 %s: %s\n",
			store->config->realStoreDir.c_str(), strerror(errno));
		return 1;
	}

	auto t0 = std::chrono::steady_clock::now();
	/* Store::addToStore minus the ceremony, with per-file hash capture
	   for the optimise pass below */
	LocalStore::ImportFileHashes fileHashes;
	std::optional<StorePath> imported;
	{
		auto sink = sourceToSink([&](Source & source) {
			imported = store->addToStoreFromDump(source, argv[2], &fileHashes);
		});
		/* sockets/fifos are skipped by dumpPath itself (NAR has no
		   representation for them; see archive.cc), which replaced
		   both the callers' destructive find -delete against live
		   roots and the lstat-per-entry PathFilter that used to live
		   here. */
		SourcePath{makeFSSourceAccessor(dir), CanonPath::root}
			.dumpPath(*sink);
		sink->finish();
	}
	auto path = *imported;
	auto t1 = std::chrono::steady_clock::now();
	progressEnd();
	fprintf(stderr, "imported: %s (%.1f s)\n", store->printStorePath(path).c_str(),
		std::chrono::duration<double>(t1 - t0).count());

	auto info = store->queryPathInfo(path);
	fprintf(stderr, "nar hash: %s\nnar size: %.1f MiB\n",
		info->narHash.to_string(HashFormat::SRI, true).c_str(),
		info->narSize / (1024.0 * 1024.0));
	if (fileHashes.dedupedFiles)
		fprintf(stderr, "streamed dedup: %lu files, %.1f MiB never hit disk\n",
			fileHashes.dedupedFiles,
			fileHashes.dedupedBytes / (1024.0 * 1024.0));

	/* the generation is now db-visible as alive, not just a name in
	   entries.cfg; deletion goes through rm-path, which unroots first */
	store->addPermRoot(path,
		store->config->stateDir / "gcroots"
			/ std::string(path.to_string()));

	OptimiseStats stats;
	auto t2 = std::chrono::steady_clock::now();
	store->optimisePath(path, stats, &fileHashes);
	auto t3 = std::chrono::steady_clock::now();
	progressEnd();
	fprintf(stderr, "optimise: linked %lu files, freed %.1f MiB (%.1f s)\n",
		stats.filesLinked, stats.bytesFreed / (1024.0 * 1024.0),
		std::chrono::duration<double>(t3 - t2).count());

	/* real (root-prefixed) path: callers use it as a directory */
	printf("%s\n", store->toRealPath(path).c_str());
	return 0;
} catch (std::exception & e) {
	/* a full store (ENOSPC) etc. must not end in std::terminate: the
	   partial temp dir cleans up on unwind, callers get a message */
	fprintf(stderr, "import-dir: %s\n", e.what());
	return 1;
}
