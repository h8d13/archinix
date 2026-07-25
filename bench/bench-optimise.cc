// Dedup benchmark: populate synthetic store, then time optimiseStore().
// Pass 1 (cold) is link-creation bound. Pass 2 (warm) is pure scan:
// loadInodeHash + per-file InodeHash lookups, the container-heavy path.
#include <chrono>
#include <cstdio>
#include <random>
#include <string>

#include <nix/store/globals.hh>
#include <nix/store/local-store.hh>
#include <nix/store/store-open.hh>
#include <nix/util/memory-source-accessor.hh>

using namespace nix;

static double timed(const char * label, auto && fn)
{
	auto t0 = std::chrono::steady_clock::now();
	fn();
	auto t1 = std::chrono::steady_clock::now();
	double s = std::chrono::duration<double>(t1 - t0).count();
	printf("%-22s %8.3f s\n", label, s);
	return s;
}

int main(int argc, char ** argv)
{
	if (argc != 5 && argc != 6) {
		fprintf(stderr, "usage: %s <store-root> <npaths> <files-per-path> <content-pool> [warm-loops]\n", argv[0]);
		return 1;
	}
	unsigned warmLoops = argc == 6 ? std::stoul(argv[5]) : 1;
	std::string root = argv[1];
	unsigned npaths = std::stoul(argv[2]);
	unsigned nfiles = std::stoul(argv[3]);
	unsigned pool = std::stoul(argv[4]);

	initLibStore();
	verbosity = lvlError;

	auto store = openStore(std::filesystem::path(root));
	auto local = store.dynamic_pointer_cast<LocalStore>();
	if (!local) {
		fprintf(stderr, "not a LocalStore\n");
		return 1;
	}

	// deterministic content pool, ~256 bytes each
	std::mt19937 rng(42);
	std::vector<std::string> contents(pool);
	for (auto & c : contents) {
		c.resize(256);
		for (auto & ch : c)
			ch = 'a' + rng() % 26;
	}

	printf("populate: %u paths x %u files, %u distinct contents\n",
		npaths, nfiles, pool);
	timed("populate (addToStore)", [&] {
		for (unsigned p = 0; p < npaths; p++) {
			auto acc = make_ref<MemorySourceAccessor>();
			for (unsigned f = 0; f < nfiles; f++)
				acc->addFile(
					CanonPath(fmt("f%06u", f)),
					// path index salts file 0 so no two store paths are identical
					f == 0 ? fmt("salt-%u", p) : contents[rng() % pool]);
			store->addToStore(fmt("bench-%u", p), {acc, CanonPath::root});
		}
	});

	OptimiseStats s1, s2;
	timed("optimise pass1 (cold)", [&] { local->optimiseStore(s1); });
	printf("  linked %lu files, freed %.1f MiB\n",
		s1.filesLinked, s1.bytesFreed / (1024.0 * 1024.0));
	timed("optimise pass2 (warm)", [&] {
		for (unsigned i = 0; i < warmLoops; i++) {
			// fresh stats per loop: report the last pass, not a sum
			OptimiseStats s;
			local->optimiseStore(s);
			s2 = s;
		}
	});
	printf("  linked %lu files, freed %.1f MiB\n",
		s2.filesLinked, s2.bytesFreed / (1024.0 * 1024.0));
	return 0;
}
