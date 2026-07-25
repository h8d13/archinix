// Import benchmark: the arch/ hot path (import-dir), not the
// whole-store walk. Same tree imported twice into fresh stores: once
// with per-file hash capture feeding optimisePath (files hashed while
// streaming, optimise reads nothing back), once without (optimise
// re-reads and re-hashes every file from disk). The delta is what the
// capture machinery buys.
// The synthetic tree is a quick smoke; real-world numbers need a real
// tree (--tree, e.g. an arch-base generation) on the target media.
// usage: bench-import <root> [nfiles] [file-kb] [content-pool]
//        bench-import <root> --tree <dir>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <random>
#include <string>

#include <nix/store/globals.hh>
#include <nix/store/local-store.hh>
#include <nix/store/store-open.hh>
#include <nix/util/memory-source-accessor.hh>
#include <nix/util/serialise.hh>
#include <nix/util/source-accessor.hh>

using namespace nix;

static double timed(const char * label, auto && fn)
{
	auto t0 = std::chrono::steady_clock::now();
	fn();
	auto t1 = std::chrono::steady_clock::now();
	double s = std::chrono::duration<double>(t1 - t0).count();
	printf("%-30s %8.3f s\n", label, s);
	return s;
}

int main(int argc, char ** argv)
{
	if (argc < 2 || argc > 5) {
		fprintf(stderr, "usage: %s <root> [nfiles] [file-kb] [content-pool]\n", argv[0]);
		return 1;
	}
	std::string root = argv[1];

	initLibStore();
	verbosity = lvlError;

	ref<SourceAccessor> acc = [&]() -> ref<SourceAccessor> {
		if (argc == 4 && strcmp(argv[2], "--tree") == 0) {
			auto dir = std::filesystem::absolute(argv[3]);
			printf("tree: %s\n", dir.c_str());
			return makeFSSourceAccessor(dir);
		}
		unsigned nfiles = argc > 2 ? std::stoul(argv[2]) : 5000;
		unsigned fileKb = argc > 3 ? std::stoul(argv[3]) : 32;
		unsigned pool = argc > 4 ? std::stoul(argv[4]) : 2500;

		// one deterministic tree, imported into both stores
		std::mt19937 rng(42);
		std::vector<std::string> contents(pool);
		for (auto & c : contents) {
			c.resize(fileKb * 1024);
			for (auto & ch : c)
				ch = 'a' + rng() % 26;
		}
		auto macc = make_ref<MemorySourceAccessor>();
		for (unsigned f = 0; f < nfiles; f++)
			macc->addFile(
				CanonPath(fmt("d%02d/e%02d/f%06u", f % 20, f % 7, f)),
				std::string(contents[rng() % pool]));
		printf("tree: %u files x %u KiB, %u distinct contents\n",
			nfiles, fileKb, pool);
		return macc;
	}();

	auto run = [&](const char * tag, bool capture) {
		auto store = openStore(std::filesystem::path(root) / tag);
		auto local = store.dynamic_pointer_cast<LocalStore>();

		LocalStore::ImportFileHashes fileHashes;
		std::optional<StorePath> imported;
		timed(fmt("import (capture %s)", tag).c_str(), [&] {
			auto sink = sourceToSink([&](Source & source) {
				imported = local->addToStoreFromDump(source, "bench",
					FileSerialisationMethod::NixArchive,
					ContentAddressMethod::Raw::NixArchive,
					HashAlgorithm::SHA256, {}, NoRepair,
					capture ? &fileHashes : nullptr);
			});
			SourcePath{acc, CanonPath::root}.dumpPath(*sink);
			sink->finish();
		});

		OptimiseStats stats;
		timed(fmt("optimise (capture %s)", tag).c_str(), [&] {
			local->optimisePath(*imported, stats,
				capture ? &fileHashes : nullptr);
		});
		printf("  linked %lu files, freed %.1f MiB\n",
			stats.filesLinked, stats.bytesFreed / (1024.0 * 1024.0));
	};

	run("off", false);
	run("on", true);
	return 0;
}
