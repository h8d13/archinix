// A/B harness for libstore internals: uses ONLY API that exists both
// at the extraction point and at HEAD (7-arg addToStoreFromDump, no
// capture; path-based optimisePath), so the same binary source builds
// against either prefix and LD_LIBRARY_PATH picks the library under
// test. Phase 1 = fresh import + optimise (cold farm). Phase 2 = same
// tree under another name (all-dup optimise, the nixgen-commit shape).
// usage: bench-ab <store-root> <tree>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>

#include <nix/store/globals.hh>
#include <nix/store/local-store.hh>
#include <nix/store/store-open.hh>
#include <nix/util/config-global.hh>
#include <nix/util/serialise.hh>
#include <nix/util/source-accessor.hh>

using namespace nix;

static double timed(const char * label, auto && fn)
{
	auto t0 = std::chrono::steady_clock::now();
	fn();
	auto t1 = std::chrono::steady_clock::now();
	double s = std::chrono::duration<double>(t1 - t0).count();
	printf("%-24s %8.3f s\n", label, s);
	return s;
}

int main(int argc, char ** argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s <store-root> <tree>\n", argv[0]);
		return 1;
	}

	initLibStore();
	verbosity = lvlError;
	globalConfig.set("build-users-group", "");
	/* the path-based optimisePath is a no-op unless auto-optimise is
	   on (upstream gate); both versions carry the setting */
	globalConfig.set("auto-optimise-store", "true");

	auto store = openStore(std::filesystem::absolute(argv[1]));
	auto local = store.dynamic_pointer_cast<LocalStore>();
	auto acc = makeFSSourceAccessor(std::filesystem::absolute(argv[2]));

	auto import = [&](const char * name) {
		std::optional<StorePath> imported;
		auto sink = sourceToSink([&](Source & source) {
			imported = local->addToStoreFromDump(source, name,
				FileSerialisationMethod::NixArchive,
				ContentAddressMethod::Raw::NixArchive,
				HashAlgorithm::SHA256, {}, NoRepair);
		});
		SourcePath{acc, CanonPath::root}.dumpPath(*sink);
		sink->finish();
		return *imported;
	};

	std::optional<StorePath> a, b;
	timed("import gen-a (cold)", [&] { a = import("gen-a"); });
	timed("optimise gen-a", [&] { local->optimisePath(local->toRealPath(*a), NoRepair); });
	timed("import gen-b (dup)", [&] { b = import("gen-b"); });
	timed("optimise gen-b (dup)", [&] { local->optimisePath(local->toRealPath(*b), NoRepair); });
	return 0;
}
