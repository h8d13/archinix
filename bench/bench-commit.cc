// Commit phases with an explicit barrier between them. import-dir
// runs import and optimise back to back, so optimise starts while the
// import's own writeback is still queued and the two costs are not
// separable from the outside. This runs the same two calls with an
// optional sync() in between (SYNC=1), which is the only way to tell
// "optimise is slow" from "optimise is waiting for the import's dirty
// pages to reach the disk". A driver-heavy generation dirties several
// GiB, so the difference is the whole story on that shape.
// usage: bench-commit <store-root> <name> <dir>    env: SYNC=1
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <unistd.h>

#include <nix/store/globals.hh>
#include <nix/store/local-store.hh>
#include <nix/store/store-open.hh>
#include <nix/util/serialise.hh>
#include <nix/util/source-accessor.hh>

using namespace nix;

static double timed(const char * label, auto && fn)
{
	auto t0 = std::chrono::steady_clock::now();
	fn();
	auto t1 = std::chrono::steady_clock::now();
	double s = std::chrono::duration<double>(t1 - t0).count();
	printf("  %-22s %7.3f s\n", label, s);
	return s;
}

int main(int argc, char ** argv)
try {
	if (argc != 4) {
		fprintf(stderr, "usage: %s <store-root> <name> <dir>\n", argv[0]);
		return 1;
	}
	bool barrier = getenv("SYNC") != nullptr;

	initLibStore();
	verbosity = lvlError;

	auto store = openStore(std::filesystem::absolute(argv[1]));
	auto local = store.dynamic_pointer_cast<LocalStore>();
	auto dir = std::filesystem::absolute(argv[3]);

	LocalStore::ImportFileHashes fileHashes;
	std::optional<StorePath> imported;

	timed("import", [&] {
		auto sink = sourceToSink([&](Source & source) {
			imported = local->addToStoreFromDump(source, argv[2],
				FileSerialisationMethod::NixArchive,
				ContentAddressMethod::Raw::NixArchive,
				HashAlgorithm::SHA256, {}, NoRepair, &fileHashes);
		});
		SourcePath{makeFSSourceAccessor(dir), CanonPath::root}.dumpPath(*sink);
		sink->finish();
	});
	printf("  files %zu, deduped %lu (%.1f MiB)\n",
		fileHashes.files.size(), fileHashes.dedupedFiles,
		fileHashes.dedupedBytes / (1024.0 * 1024.0));

	if (barrier)
		timed("barrier (sync)", [] { ::sync(); });

	OptimiseStats stats;
	timed("optimise", [&] { local->optimisePath(*imported, stats, &fileHashes); });
	printf("  linked %lu files, freed %.1f MiB\n",
		stats.filesLinked, stats.bytesFreed / (1024.0 * 1024.0));

	timed("trailing sync", [] { ::sync(); });
	return 0;
} catch (std::exception & e) {
	fprintf(stderr, "bench-commit: %s\n", e.what());
	return 1;
}
