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
#include <nix/util/config-global.hh>

using namespace nix;

int main(int argc, char ** argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <store-root> <store-path-basename>...\n", argv[0]);
		return 1;
	}

	initLibStore(false);
	verbosity = lvlError;
	globalConfig.set("build-users-group", "");

	/* openStore aborts on a relative root */
	auto store = openStore("local?root=" + std::filesystem::absolute(argv[1]).string());
	auto & gcStore = require<GcStore>(*store);

	auto & fsStore = require<LocalFSStore>(*store);
	auto gcroots = fsStore.config.stateDir.get().path() / "gcroots";

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
}
