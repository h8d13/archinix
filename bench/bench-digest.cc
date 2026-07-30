// Is the NAR digest worth parallelising? SHA-256 over one message is
// sequential by construction, so the only ways to go faster are more
// streams at once (multi-buffer, which needs several independent
// messages) or a different hash function (a tree hash over chunks,
// which is a DIFFERENT digest: the NAR hash IS the store path, so that
// is a format change, not an optimisation).
//
// This measures the ceiling both ideas aim at, so the decision is made
// against numbers rather than against the idea:
//   linear     one SHA-256 over the whole buffer, one thread. What an
//              import actually does today.
//   leaves     T threads each hashing their own slice: the aggregate
//              rate a multi-buffer or tree-hash leaf phase could reach.
//   tree       leaves plus the combining digest over the T leaf
//              digests, wall clock end to end.
// Run it twice, once with OPENSSL_ia32cap=":~0x20000000" to mask
// SHA-NI: with the extension the per-core rate is high enough that the
// restore side of an import is the limit, without it the digest is, and
// that is the case where parallelising would pay.
//
// EVP, not the SHA256_Init/Update/Final trio: those are deprecated as
// of OpenSSL 3.0 and only compile with a warning suppressed. libutil's
// hash.cc still calls them, which is drift of its own.
// usage: bench-digest [MiB] [max-threads]
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <openssl/evp.h>

static double gibs(size_t bytes, double seconds)
{
	return bytes / seconds / (1024.0 * 1024.0 * 1024.0);
}

static double now()
{
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct Digest
{
	std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx;

	Digest()
		: ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free)
	{
		if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
			abort();
	}

	/* 64 KiB at a time, the order of a NAR chunk off the coroutine
	   spine, so the per-call overhead matches the real caller's */
	void run(const unsigned char * p, size_t len)
	{
		constexpr size_t chunk = 64 * 1024;
		while (len) {
			size_t n = len < chunk ? len : chunk;
			if (EVP_DigestUpdate(ctx.get(), p, n) != 1)
				abort();
			p += n;
			len -= n;
		}
	}

	void finish(unsigned char out[32])
	{
		unsigned int outLen = 0;
		if (EVP_DigestFinal_ex(ctx.get(), out, &outLen) != 1 || outLen != 32)
			abort();
	}
};

static void hashRange(const unsigned char * p, size_t len, unsigned char out[32])
{
	Digest d;
	d.run(p, len);
	d.finish(out);
}

int main(int argc, char ** argv)
{
	size_t mib = argc > 1 ? strtoul(argv[1], nullptr, 10) : 1024;
	unsigned maxThreads = argc > 2 ? strtoul(argv[2], nullptr, 10) : 16;
	size_t bytes = mib * 1024 * 1024;

	printf("buffer %zu MiB, hw threads %u, OPENSSL_ia32cap=%s\n", mib,
	       std::thread::hardware_concurrency(),
	       getenv("OPENSSL_ia32cap") ? getenv("OPENSSL_ia32cap") : "(unset)");

	std::vector<unsigned char> buf(bytes);
	/* deterministic filler, cheap: content does not affect SHA-256 cost */
	for (size_t i = 0; i < bytes; i += 4096)
		memset(buf.data() + i, (int) (i / 4096) & 0xff,
		       4096 < bytes - i ? 4096 : bytes - i);

	unsigned char digest[32];
	/* one warm pass so the buffer is faulted in and the clocks are up */
	hashRange(buf.data(), bytes, digest);

	double best = 1e9;
	for (int rep = 0; rep < 3; rep++) {
		double t0 = now();
		hashRange(buf.data(), bytes, digest);
		double dt = now() - t0;
		if (dt < best)
			best = dt;
	}
	double linear = gibs(bytes, best);
	printf("  %-28s %7.3f s  %6.2f GiB/s\n", "linear (1 thread)", best, linear);

	for (unsigned t = 2; t <= maxThreads; t *= 2) {
		size_t slice = bytes / t;
		std::vector<std::array<unsigned char, 32>> leaves(t);
		double bestLeaves = 1e9, bestTree = 1e9;
		for (int rep = 0; rep < 3; rep++) {
			double t0 = now();
			std::vector<std::thread> ths;
			for (unsigned i = 0; i < t; i++)
				ths.emplace_back([&, i] {
					size_t len = i + 1 == t ? bytes - i * slice : slice;
					hashRange(buf.data() + i * slice, len, leaves[i].data());
				});
			for (auto & th : ths)
				th.join();
			double leafDt = now() - t0;
			/* the combining digest: T * 32 bytes, so its cost is
			   the thread join plus a rounding error */
			Digest combine;
			for (auto & l : leaves)
				combine.run(l.data(), l.size());
			combine.finish(digest);
			double treeDt = now() - t0;
			if (leafDt < bestLeaves)
				bestLeaves = leafDt;
			if (treeDt < bestTree)
				bestTree = treeDt;
		}
		printf("  leaves T=%-2u                  %7.3f s  %6.2f GiB/s  (%.2fx)\n",
		       t, bestLeaves, gibs(bytes, bestLeaves),
		       gibs(bytes, bestLeaves) / linear);
		printf("  tree   T=%-2u (leaves+combine) %7.3f s  %6.2f GiB/s  (%.2fx)\n",
		       t, bestTree, gibs(bytes, bestTree),
		       gibs(bytes, bestTree) / linear);
	}
	return 0;
}
