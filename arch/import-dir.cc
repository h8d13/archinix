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
// usage: import-dir <store-root> <name> <dir>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>

#include <sys/stat.h>

#include <nix/store/globals.hh>
#include <nix/store/local-store.hh>
#include <nix/store/store-open.hh>
#include <nix/util/archive.hh>
#include <nix/util/config-global.hh>
#include <nix/util/serialise.hh>
#include <nix/util/source-accessor.hh>

using namespace nix;

int main(int argc, char ** argv)
try {
	if (argc != 4) {
		fprintf(stderr, "usage: %s <store-root> <name> <dir>\n", argv[0]);
		return 1;
	}

	initLibStore(false);
	verbosity = lvlError;

	/* we never build derivations, and under a fake-root userns the
	   root default "nixbld" doesn't exist */
	globalConfig.set("build-users-group", "");

	/* openStore aborts on a relative root */
	auto store = openStore("local?root=" + std::filesystem::absolute(argv[1]).string());
	auto local = store.dynamic_pointer_cast<LocalStore>();
	auto dir = std::filesystem::absolute(argv[3]);

	auto t0 = std::chrono::steady_clock::now();
	/* Store::addToStore minus the ceremony, with per-file hash capture
	   for the optimise pass below */
	LocalStore::ImportFileHashes fileHashes;
	std::optional<StorePath> imported;
	{
		auto sink = sourceToSink([&](Source & source) {
			imported = local->addToStoreFromDump(source, argv[2],
				FileSerialisationMethod::NixArchive,
				ContentAddressMethod::Raw::NixArchive,
				HashAlgorithm::SHA256, {}, NoRepair, &fileHashes);
		});
		/* NAR has no representation for sockets/fifos; filtering them
		   out of the dump replaces the callers' destructive
		   find -delete against the live root. The filter sees paths
		   relative to the accessor root ("/x/y"). */
		PathFilter noSpecials = [&](const std::string & p) {
			struct stat st;
			if (::lstat((dir.native() + p).c_str(), &st) == -1)
				return true; /* let the dump report it */
			return !(S_ISSOCK(st.st_mode) || S_ISFIFO(st.st_mode));
		};
		SourcePath{makeFSSourceAccessor(dir), CanonPath::root}
			.dumpPath(*sink, noSpecials);
		sink->finish();
	}
	auto path = *imported;
	auto t1 = std::chrono::steady_clock::now();
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
	local->addPermRoot(path,
		local->config->stateDir.get().path() / "gcroots"
			/ std::string(path.to_string()));

	OptimiseStats stats;
	auto t2 = std::chrono::steady_clock::now();
	local->optimisePath(path, stats, &fileHashes);
	auto t3 = std::chrono::steady_clock::now();
	fprintf(stderr, "optimise: linked %lu files, freed %.1f MiB (%.1f s)\n",
		stats.filesLinked, stats.bytesFreed / (1024.0 * 1024.0),
		std::chrono::duration<double>(t3 - t2).count());

	/* real (root-prefixed) path: callers use it as a directory */
	printf("%s\n", local->toRealPath(path).c_str());
	return 0;
} catch (std::exception & e) {
	/* a full store (ENOSPC) etc. must not end in std::terminate: the
	   partial temp dir cleans up on unwind, callers get a message */
	fprintf(stderr, "import-dir: %s\n", e.what());
	return 1;
}
