// A/B harness for libstore internals: sticks to the narrow API the
// store tools themselves use (addToStoreFromDump without capture,
// path-based optimisePath), so the same source builds against two
// prefixes of this fork and LD_LIBRARY_PATH picks the library under
// test. Any header change still breaks it, so build both prefixes from
// commits that share the API you are measuring.
// Phase 1 = fresh import + optimise (cold farm). Phase 2 = same tree
// under another name (all-dup optimise, the nixgen-commit shape).
// usage: bench-ab <store-root> <tree>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>

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

	/* the path-based optimisePath is a no-op unless auto-optimise is
	   on, and the setting is per store config now (there is no global
	   nix.conf here), so build the config rather than calling
	   openStore() */
	auto config = make_ref<LocalStore::Config>(std::filesystem::absolute(argv[1]));
	config->localSettings.autoOptimiseStore = true;
	auto store = config->openStore();
	auto acc = makeFSSourceAccessor(std::filesystem::absolute(argv[2]));

	auto import = [&](const char * name) {
		std::optional<StorePath> imported;
		auto sink = sourceToSink([&](Source & source) {
			imported = store->addToStoreFromDump(source, name);
		});
		SourcePath{acc, CanonPath::root}.dumpPath(*sink);
		sink->finish();
		return *imported;
	};

	std::optional<StorePath> a, b;
	timed("import gen-a (cold)", [&] { a = import("gen-a"); });
	timed("optimise gen-a", [&] { store->optimisePath(store->toRealPath(*a)); });
	timed("import gen-b (dup)", [&] { b = import("gen-b"); });
	timed("optimise gen-b (dup)", [&] { store->optimisePath(store->toRealPath(*b)); });
	return 0;
}
