// Byte-heavy bench: the libutil half of an import, swept over file
// size at constant total bytes. BASELINE's shapes all vary file
// *count* (800k small files), where the cost is tokens and per-file
// metadata. A generation carrying drivers/firmware/debug objects
// varies file *size* instead: same commit, two orders of magnitude
// fewer files, every byte still copied and hashed. Holding the total
// constant and moving only the size is what separates a per-file cost
// from a per-byte one -- a flat MiB/s across the sweep means size is
// not the variable, a falling one names the stage that is.
//
// Stages, each adding one layer of the real import to the previous:
//   dump->null      source read + NAR framing (the floor)
//   dump->sha256    + one whole-stream digest, inline
//   dump->s2s->null + the sourceToSink thread boundary (ring copies)
//   dump->s2s->restore  + parseDump and the writes: libutil's whole
//                       import path, no store db, no dedup, no
//                       second (per-file) hash
//   flush           sync() after the restore, timed apart: an import
//                   that "finishes" with gigabytes still dirty has
//                   only handed the bill to the next writer
//
// COLD=1 evicts the source tree's data pages before every timed
// stage (same fadvise trick as bench/evict-cache.py, in-process), so
// reads cost what they cost on a box committing hours after boot.
// The synthetic sweep isolates size; --tree runs the same stages over
// a real generation (firmware, kernel modules, gpu drivers, the mixed
// shape a full Arch rootfs actually has), which is the only thing
// that reproduces a real nixgen-commit.
// usage: bench-large <workdir> [total-mib] [sizes-mib...]
//        bench-large <workdir> --tree <dir>...
//        env: COLD=1
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

#include <nix/util/archive.hh>
#include <nix/util/file-system.hh>
#include <nix/util/hash.hh>
#include <nix/util/serialise.hh>
#include <nix/util/source-accessor.hh>
#include <nix/util/source-path.hh>

using namespace nix;

static bool cold = false;

/* Drop a tree's data pages. Dentries and inodes stay cached (dropping
   those needs root), so this is the same partial cold as
   bench/evict-cache.py: real after-boot cold is worse. */
static void evict(const std::filesystem::path & dir)
{
	::sync();
	for (auto & e : std::filesystem::recursive_directory_iterator(dir)) {
		if (!e.is_regular_file())
			continue;
		int fd = ::open(e.path().c_str(), O_RDONLY);
		if (fd < 0)
			continue;
		::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
		::close(fd);
	}
}

struct Timing
{
	double s;
	double mibs;
};

static Timing timed(const char * label, uint64_t bytes, auto && fn)
{
	auto t0 = std::chrono::steady_clock::now();
	fn();
	auto t1 = std::chrono::steady_clock::now();
	double s = std::chrono::duration<double>(t1 - t0).count();
	double mibs = bytes / (1024.0 * 1024.0) / s;
	printf("  %-22s %7.3f s   %8.1f MiB/s\n", label, s, mibs);
	return {s, mibs};
}

/* One tree of `count` files of `fileBytes` each. Incompressible and
   distinct, so nothing dedups and no stage can skip work it would do
   on a real driver blob. */
static uint64_t makeTree(const std::filesystem::path & dir, unsigned count, uint64_t fileBytes)
{
	std::filesystem::create_directories(dir);
	constexpr size_t chunk = 1 << 20;
	std::vector<char> buf(chunk);
	std::mt19937_64 rng(1234);
	uint64_t total = 0;

	for (unsigned f = 0; f < count; f++) {
		auto p = dir / fmt("blob%04u.bin", f);
		int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0)
			throw SysError("creating %s", p.native());
		uint64_t left = fileBytes;
		while (left) {
			for (size_t i = 0; i < chunk; i += 8) {
				uint64_t v = rng();
				memcpy(buf.data() + i, &v, 8);
			}
			size_t n = std::min<uint64_t>(left, chunk);
			if (::write(fd, buf.data(), n) != (ssize_t) n)
				throw SysError("writing %s", p.native());
			left -= n;
			total += n;
		}
		::close(fd);
	}
	::sync();
	return total;
}

/* Restored trees are 0444/0555 under canonical mode, and the dirs
   0755 with mtime 1; make them writable so remove_all can work. */
static void unprotect(const std::filesystem::path & dst)
{
	::chmod(dst.c_str(), 0755);
	for (auto & e : std::filesystem::recursive_directory_iterator(dst))
		if (e.is_directory())
			::chmod(e.path().c_str(), 0755);
}

/* Read every byte once so stage 1 does not eat the cold-read cost the
   other four then get for free (a real tree measured 1470 MiB/s on
   dump->null and 3510 MiB/s on dump->sha256, which is above the
   hash's own ceiling: that was cache temperature, not code). COLD=1
   wants the opposite and evicts per stage instead. */
static void prewarm(const std::filesystem::path & dir)
{
	std::vector<char> buf(1 << 20);
	for (auto & e : std::filesystem::recursive_directory_iterator(dir)) {
		if (!e.is_regular_file())
			continue;
		int fd = ::open(e.path().c_str(), O_RDONLY);
		if (fd < 0)
			continue;
		while (::read(fd, buf.data(), buf.size()) > 0)
			;
		::close(fd);
	}
}

static void runStages(const std::filesystem::path & tree, const std::filesystem::path & dst, uint64_t bytes)
{
	SourcePath src{makeFSSourceAccessor(tree), CanonPath::root};

	if (!cold)
		prewarm(tree);

	if (cold)
		evict(tree);
	timed("dump->null", bytes, [&] {
		NullSink sink;
		src.dumpPath(sink);
	});

	if (cold)
		evict(tree);
	timed("dump->sha256", bytes, [&] {
		HashSink sink{HashAlgorithm::SHA256};
		src.dumpPath(sink);
		sink.finish();
	});

	if (cold)
		evict(tree);
	timed("dump->s2s->null", bytes, [&] {
		auto sink = sourceToSink([&](Source & source) {
			NullSink null;
			source.drainInto(null);
		});
		src.dumpPath(*sink);
		sink->finish();
	});

	if (cold)
		evict(tree);
	std::filesystem::remove_all(dst);
	timed("dump->s2s->restore", bytes, [&] {
		auto sink = sourceToSink([&](Source & source) {
			restorePath(dst, source, /*startFsync=*/false,
				/*canonical=*/true);
		});
		src.dumpPath(*sink);
		sink->finish();
	});
	/* the restore above returns with the writes still in page cache;
	   whoever syncs next pays for them, so charge it here rather
	   than to the next bench point */
	timed("flush (sync)", bytes, [&] { ::sync(); });

	unprotect(dst);
	std::filesystem::remove_all(dst);
}

/* Apparent size of a tree's regular files, which is what the NAR
   carries (du reports allocated blocks instead). is_regular_file
   follows symlinks, and a store tree is symlink-heavy -- counting
   lib.so as a second copy of lib.so.1 inflated a real tree by 30% and
   with it every MiB/s below, so the type comes off the link itself. */
static uint64_t treeBytes(const std::filesystem::path & dir)
{
	uint64_t b = 0;
	std::error_code ec;
	for (auto it = std::filesystem::recursive_directory_iterator(
		     dir, std::filesystem::directory_options::skip_permission_denied, ec);
	     it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
		auto st = it->symlink_status(ec);
		if (std::filesystem::is_regular_file(st))
			b += it->file_size(ec);
	}
	return b;
}

int main(int argc, char ** argv)
try {
	if (argc < 2) {
		fprintf(stderr, "usage: %s <workdir> [total-mib] [sizes-mib...]\n", argv[0]);
		fprintf(stderr, "       %s <workdir> --tree <dir>...\n", argv[0]);
		return 1;
	}
	cold = getenv("COLD") != nullptr;

	std::filesystem::path work = std::filesystem::absolute(argv[1]);
	std::filesystem::create_directories(work);

	if (argc > 3 && strcmp(argv[2], "--tree") == 0) {
		printf("cache %s\n", cold ? "COLD (evicted per stage)" : "warm");
		for (int i = 3; i < argc; i++) {
			auto tree = std::filesystem::absolute(argv[i]);
			uint64_t bytes = treeBytes(tree);
			printf("\n--- %s (%llu MiB)\n", tree.c_str(),
				(unsigned long long) (bytes >> 20));
			runStages(tree, work / "out-tree", bytes);
		}
		return 0;
	}

	uint64_t totalMib = argc > 2 ? std::stoull(argv[2]) : 1024;

	std::vector<uint64_t> sizes;
	for (int i = 3; i < argc; i++)
		sizes.push_back(std::stoull(argv[i]));
	/* default sweep: a kernel module, a mesa/llvm .so, an nvidia
	   blob, a firmware pack -- against the small-file end that
	   BASELINE already covers */
	if (sizes.empty())
		sizes = {1, 8, 64, 256};

	printf("total %llu MiB per point, cache %s\n",
		(unsigned long long) totalMib, cold ? "COLD (evicted per stage)" : "warm");

	for (auto sizeMib : sizes) {
		uint64_t fileBytes = sizeMib * 1024 * 1024;
		unsigned count = (unsigned) std::max<uint64_t>(1, totalMib / sizeMib);
		auto tree = work / fmt("tree-%llu", (unsigned long long) sizeMib);

		uint64_t bytes = makeTree(tree, count, fileBytes);
		printf("\n--- %u files x %llu MiB (%llu MiB total)\n",
			count, (unsigned long long) sizeMib,
			(unsigned long long) (bytes >> 20));

		runStages(tree, work / fmt("out-%llu", (unsigned long long) sizeMib), bytes);
		std::filesystem::remove_all(tree);
	}
	return 0;
} catch (std::exception & e) {
	fprintf(stderr, "bench-large: %s\n", e.what());
	return 1;
}
