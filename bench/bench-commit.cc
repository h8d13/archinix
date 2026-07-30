// Commit phases with an explicit barrier between them. import-dir
// runs import and optimise back to back, so optimise starts while the
// import's own writeback is still queued and the two costs are not
// separable from the outside. This runs the same two calls with an
// optional sync() in between (SYNC=1), which is the only way to tell
// "optimise is slow" from "optimise is waiting for the import's dirty
// pages to reach the disk". A driver-heavy generation dirties several
// GiB, so the difference is the whole story on that shape.
//
// Each phase also reports its own resident-set peak, sampled while the
// phase runs (VmHWM is a process-lifetime high-water mark and cannot
// say which phase paid for it). RSS is anon + mapped file pages: the
// dirty NAR data a commit leaves in the page cache is NOT in here, it
// is charged to the cgroup instead (bench/vm-commit.sh measures that
// side in the box). What this catches is the library holding a tree in
// memory: import streams, so its curve should be flat in tree size,
// while optimise carries the per-path hash tables.
// CAPTURE=0 drops the per-file hash capture: it is the one structure
// that scales with the tree (see BASELINE), so the difference between
// the two runs is what capture costs in RAM.
// The writer thread runs at SCHED_RR where CAP_SYS_NICE allows it, as
// in import-dir.
// DEDUPT=n threads issuing the dedup swaps (0 = inline on the hashing
// thread, the shape this replaced). A plain field on the store config,
// so A/B is one build rather than two prefixes.
// usage: bench-commit <store-root> <name> <dir>
//        env: SYNC=1 CAPTURE=0 DEDUPT=n
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <thread>
#include <unistd.h>

#include <nix/store/globals.hh>
#include <nix/store/local-store.hh>
#include <nix/store/store-open.hh>
#include <nix/util/scheduling.hh>
#include <nix/util/serialise.hh>
#include <nix/util/source-accessor.hh>

using namespace nix;

// statm field 2 is resident pages; /proc/self/status VmHWM would be one
// read but only gives the whole-run peak
static size_t rssKiB()
{
	FILE * f = fopen("/proc/self/statm", "r");
	if (!f)
		return 0;
	unsigned long total = 0, resident = 0;
	int n = fscanf(f, "%lu %lu", &total, &resident);
	fclose(f);
	if (n != 2)
		return 0;
	return resident * (sysconf(_SC_PAGESIZE) / 1024);
}

// 10 ms: fine enough to catch a hash-table build, coarse enough that
// the sampler itself stays off the profile
static double timed(const char * label, auto && fn)
{
	std::atomic<size_t> peak{rssKiB()};
	std::atomic<bool> done{false};
	std::thread sampler([&] {
		while (!done.load(std::memory_order_relaxed)) {
			auto rss = rssKiB();
			if (rss > peak.load(std::memory_order_relaxed))
				peak.store(rss, std::memory_order_relaxed);
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	});

	auto t0 = std::chrono::steady_clock::now();
	fn();
	auto t1 = std::chrono::steady_clock::now();

	done.store(true, std::memory_order_relaxed);
	sampler.join();

	double s = std::chrono::duration<double>(t1 - t0).count();
	printf("  %-22s %7.3f s   rss peak %6.1f MiB (end %6.1f)\n", label, s,
		peak.load() / 1024.0, rssKiB() / 1024.0);
	return s;
}

int main(int argc, char ** argv)
try {
	if (argc != 4) {
		fprintf(stderr, "usage: %s <store-root> <name> <dir>\n", argv[0]);
		return 1;
	}
	bool barrier = getenv("SYNC") != nullptr;
	const char * cap = getenv("CAPTURE");
	bool capture = !cap || strcmp(cap, "0") != 0;

	initLibStore();
	verbosity = lvlWarn;
	realtimeWriter = true;

	// opened through its own config, not the one-argument openStore():
	// store knobs are plain fields there, which is the only way in
	auto config = make_ref<LocalStore::Config>(std::filesystem::absolute(argv[1]));
	if (const char * d = getenv("DEDUPT"))
		config->localSettings.dedupThreads = strtoul(d, nullptr, 10);
	auto store = config->openStore();
	auto dir = std::filesystem::absolute(argv[3]);

	LocalStore::ImportFileHashes fileHashes;
	auto * hashes = capture ? &fileHashes : nullptr;
	std::optional<StorePath> imported;

	timed("import", [&] {
		auto sink = sourceToSink([&](Source & source) {
			imported = store->addToStoreFromDump(source, argv[2], hashes);
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
	timed("optimise", [&] { store->optimisePath(*imported, stats, hashes); });
	printf("  linked %lu files, freed %.1f MiB\n",
		stats.filesLinked, stats.bytesFreed / (1024.0 * 1024.0));

	timed("trailing sync", [] { ::sync(); });
	// the captured hashes are the one structure that scales with the
	// tree, so name its size next to the peak it lands in
	printf("  fileHashes %zu entries (capture %s)\n",
		fileHashes.files.size(), capture ? "on" : "off");
	return 0;
} catch (std::exception & e) {
	fprintf(stderr, "bench-commit: %s\n", e.what());
	return 1;
}
