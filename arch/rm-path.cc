// Delete store paths from a local store, disk and db together, via the
// GC codepath: refuses paths that other valid paths still reference.
// Import-dir registers each generation as a GC root; the root is
// dropped here first, so deletion stays a single deliberate operation
// while the store db protects rooted paths from everything else.
// usage: rm-path <store-root> <store-path-basename>...
#include <cstdio>
#include <filesystem>

#include <nix/store/gc-store.hh>
#include <nix/store/globals.hh>
#include <nix/store/local-fs-store.hh>
#include <nix/store/store-cast.hh>
#include <nix/store/store-open.hh>

using namespace nix;

int main(int argc, char ** argv)
try {
	if (argc < 3) {
		fprintf(stderr, "usage: %s <store-root> <store-path-basename>...\n", argv[0]);
		return 1;
	}

	initLibStore();
	verbosity = lvlError;

	auto store = openStore(std::filesystem::absolute(argv[1]));
	auto & gcStore = require<GcStore>(*store);

	auto & fsStore = require<LocalFSStore>(*store);
	auto gcroots = fsStore.config.stateDir / "gcroots";

	GCOptions::SpecificPaths specific;
	for (int i = 2; i < argc; i++) {
		StorePath path{argv[i]};
		/* pre-roots generations have no link: ENOENT is fine */
		std::error_code ec;
		std::filesystem::remove(gcroots / std::string(path.to_string()), ec);
		specific.paths.insert(std::move(path));
	}

	GCOptions opts;
	opts.action = GCOptions::gcDeleteSpecific;
	opts.pathsToDelete = std::move(specific);

	GCResults results;
	gcStore.collectGarbage(opts, results);
	printf("deleted %zu paths, freed %.1f MiB\n",
		results.paths.size(), results.bytesFreed / (1024.0 * 1024.0));
	return 0;
} catch (std::exception & e) {
	/* a bad basename, an unreadable store or a full disk must not
	   end in std::terminate: callers get a message and rc 1 */
	fprintf(stderr, "rm-path: %s\n", e.what());
	return 1;
}
