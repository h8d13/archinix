// Regression test for parallel optimiseStore.
// Hazards exercised: two optimiseStore runs racing on the same store,
// threads racing on identical content (same link farm entry), and a
// GC-like chaos thread unlinking .links entries mid-run.
// Safety properties checked: file contents survive byte-for-byte,
// permissions are restored, dedup converges, repeat pass is a no-op.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include <nix/store/globals.hh>
#include <nix/store/local-store.hh>
#include <nix/store/store-open.hh>
#include <nix/util/memory-source-accessor.hh>

using namespace nix;
namespace fs = std::filesystem;

static int testNum = 0, failures = 0;

static void ok(bool cond, const std::string & desc, const std::string & detail = "")
{
	testNum++;
	if (cond)
		printf("ok %d - %s\n", testNum, desc.c_str());
	else {
		printf("not ok %d - %s%s%s\n", testNum, desc.c_str(),
			detail.empty() ? "" : ": ", detail.c_str());
		failures++;
	}
}

static std::string readFileStr(const fs::path & p)
{
	std::ifstream f(p, std::ios::binary);
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

// store-path contents: absolute file path -> content (dirs skipped)
static std::map<std::string, std::string> walkStore(const fs::path & storeDir)
{
	std::map<std::string, std::string> out;
	for (auto & ent : fs::directory_iterator(storeDir)) {
		auto name = ent.path().filename().string();
		if (name == ".links" || name.starts_with(".tmp-link"))
			continue;
		if (ent.is_regular_file())
			out[ent.path().string()] = readFileStr(ent.path());
		else if (ent.is_directory())
			for (auto & f : fs::recursive_directory_iterator(ent.path()))
				if (f.is_regular_file())
					out[f.path().string()] = readFileStr(f.path());
	}
	return out;
}

int main(int argc, char ** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <store-root>\n", argv[0]);
		return 1;
	}
	std::string root = argv[1];
	fs::path storeDir = root + "/nix/store";
	fs::path linksDir = storeDir / ".links";

	initLibStore();
	verbosity = lvlError;

	auto store = openStore(std::filesystem::path(root));
	auto local = store.dynamic_pointer_cast<LocalStore>();

	/* Populate: heavy cross-path duplication (every thread wants the
	   same link farm entries), nested dirs (permission toggling). */
	std::mt19937 rng(1337);
	std::vector<std::string> pool(20);
	for (unsigned i = 0; i < pool.size(); i++)
		pool[i] = "content-" + std::to_string(i) + "-"
			+ std::string(200 + i, 'a' + i % 26);

	for (unsigned p = 0; p < 40; p++) {
		auto acc = make_ref<MemorySourceAccessor>();
		for (unsigned f = 0; f < 50; f++) {
			auto dir = fmt("d%d/e%d", f % 4, f % 2);
			acc->addFile(CanonPath(fmt("%s/f%03d", dir, f)),
				std::string(pool[rng() % pool.size()]));
		}
		acc->addFile(CanonPath("top"), fmt("unique-%d", p));
		// identical empty content in every path: must never dedup
		// (empty-file skip; welding runtime-mutable paths is the hazard)
		acc->addFile(CanonPath("d0/empty"), "");
		store->addToStore(fmt("t%d", p), {acc, CanonPath::root});
	}

	auto expected = walkStore(storeDir);
	ok(expected.size() == 40 * 52, "populated store",
		fmt("%d files", expected.size()));

	/* Two full optimise runs racing each other, plus a chaos thread
	   unlinking link farm entries (concurrent GC simulation). */
	std::atomic<bool> chaosStop{false};
	std::atomic<int> optimiseErrors{0};
	std::thread chaos([&] {
		std::mt19937 crng(7);
		while (!chaosStop) {
			AutoCloseDir dir(opendir(linksDir.string().c_str()));
			if (dir) {
				struct dirent * de;
				while ((de = readdir(dir.get())))
					if (de->d_name[0] != '.' && crng() % 3 == 0)
						::unlink((linksDir / de->d_name).c_str());
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	});

	auto runOptimise = [&] {
		try {
			OptimiseStats s;
			local->optimiseStore(s);
		} catch (std::exception & e) {
			fprintf(stderr, "optimise threw: %s\n", e.what());
			optimiseErrors++;
		}
	};
	{
		std::thread a(runOptimise), b(runOptimise);
		a.join();
		b.join();
	}
	chaosStop = true;
	chaos.join();

	ok(optimiseErrors == 0, "concurrent optimise + chaos unlink: no exceptions");

	auto after = walkStore(storeDir);
	ok(after == expected, "contents intact after chaos run");

	/* Clean converge: one pass relinks whatever chaos delinked, the
	   next must be a no-op. */
	OptimiseStats clean1, clean2;
	local->optimiseStore(clean1);
	local->optimiseStore(clean2);
	ok(clean2.filesLinked == 0, "optimise converges",
		fmt("second clean pass linked %d", clean2.filesLinked));

	ok(walkStore(storeDir) == expected, "contents intact after converge");

	/* Full dedup: every non-empty content group collapsed to one
	   inode; empty files are exempt by design (never farmed). */
	std::map<std::string, std::set<ino_t>> groups;
	bool statFailed = false;
	unsigned emptyLinked = 0, emptyFiles = 0;
	for (auto & [p, content] : after) {
		struct stat st;
		if (::lstat(p.c_str(), &st) != 0) {
			statFailed = true;
			continue;
		}
		if (content.empty()) {
			emptyFiles++;
			if (st.st_nlink != 1)
				emptyLinked++;
		} else
			groups[content].insert(st.st_ino);
	}
	unsigned multi = 0;
	for (auto & [content, inos] : groups)
		if (inos.size() > 1)
			multi++;
	ok(!statFailed && multi == 0, "full dedup: one inode per distinct content",
		fmt("%d groups with >1 inode", multi));
	ok(emptyFiles == 40 && emptyLinked == 0,
		"empty files stay unlinked (nlink 1 each)",
		fmt("%d empty seen, %d hard-linked", emptyFiles, emptyLinked));

	/* and the farm never took an empty entry */
	unsigned zeroLinks = 0;
	for (auto & ent : fs::directory_iterator(linksDir))
		if (ent.is_regular_file() && ent.file_size() == 0)
			zeroLinks++;
	ok(zeroLinks == 0, "link farm holds no empty entries",
		fmt("%d zero-size entries", zeroLinks));

	/* Single-path optimise, which fans out one task per child
	   directory. This is the shape a commit runs (nixgen-commit
	   optimises exactly the path it just imported), and unlike
	   optimiseStore above the concurrency is *inside* one path: sibling
	   subtrees toggle their own directories' modes while racing each
	   other for the same farm entries. Wide and deep on purpose, so the
	   walk actually forks instead of running one task.
	   The canonical-mode sweep below covers this path too. */
	{
		auto acc = make_ref<MemorySourceAccessor>();
		for (unsigned d = 0; d < 24; d++)
			for (unsigned f = 0; f < 20; f++)
				acc->addFile(CanonPath(fmt("sub%02d/deep/f%02d", d, f)),
					std::string(pool[(d * 20 + f) % pool.size()]));
		// a file directly under the root: its parent is the one
		// directory the fan-out does not hand to a task
		acc->addFile(CanonPath("rootfile"), "wide-root-content");
		auto wide = store->addToStore("wide", {acc, CanonPath::root});
		auto wideDir = storeDir / wide.to_string();

		std::map<std::string, std::string> wideBefore;
		for (auto & f : fs::recursive_directory_iterator(wideDir))
			if (f.is_regular_file())
				wideBefore[f.path().string()] = readFileStr(f.path());

		OptimiseStats w1, w2;
		local->optimisePath(wide, w1);
		local->optimisePath(wide, w2);

		ok(w1.filesLinked > 0, "single-path optimise links",
			fmt("linked %d", w1.filesLinked));
		ok(w2.filesLinked == 0, "single-path optimise converges",
			fmt("second pass linked %d", w2.filesLinked));

		std::map<std::string, std::string> wideAfter;
		for (auto & f : fs::recursive_directory_iterator(wideDir))
			if (f.is_regular_file())
				wideAfter[f.path().string()] = readFileStr(f.path());
		ok(wideAfter == wideBefore, "single-path contents intact",
			fmt("%d files before, %d after",
				wideBefore.size(), wideAfter.size()));

		/* Every distinct content in the path must end on one inode:
		   subtrees racing for the same farm entry must not each keep
		   their own copy. */
		std::map<std::string, std::set<ino_t>> wideGroups;
		for (auto & [p, content] : wideAfter) {
			struct stat st;
			if (::lstat(p.c_str(), &st) == 0)
				wideGroups[content].insert(st.st_ino);
		}
		unsigned wideMulti = 0;
		for (auto & [content, inos] : wideGroups)
			if (inos.size() > 1)
				wideMulti++;
		ok(wideMulti == 0, "single-path dedup: one inode per content",
			fmt("%d groups with >1 inode", wideMulti));
	}

	/* Permissions restored. optimisePath_ chmods a directory +w to swap
	   a file in and leans on ~MakeReadOnly to put the mode back, so a
	   leak here is a real hazard. Assert the canonical mode itself
	   (posix-fs-canonicalise.cc: dirs 0755, files 0555 if executable
	   else 0444) rather than "nothing writable": store dirs are 0755
	   by design, so an owner-writable dir is correct, and a leak shows
	   up as a file left at 0644. */
	unsigned offCanon = 0;
	std::string firstBad;
	auto checkCanon = [&](const fs::path & p) {
		struct stat st;
		if (::lstat(p.c_str(), &st) != 0 || S_ISLNK(st.st_mode))
			return;
		mode_t mode = st.st_mode & ~S_IFMT;
		mode_t canon = S_ISDIR(st.st_mode) ? 0755
			: (mode & S_IXUSR)           ? 0555
						     : 0444;
		if (mode == canon)
			return;
		if (!offCanon)
			firstBad = fmt("%s is %04o, want %04o",
				p.string(), mode, canon);
		offCanon++;
	};
	for (auto & ent : fs::directory_iterator(storeDir)) {
		auto name = ent.path().filename().string();
		if (name == ".links" || name.starts_with(".tmp-link"))
			continue;
		checkCanon(ent.path());
		if (ent.is_directory())
			for (auto & f : fs::recursive_directory_iterator(ent.path()))
				checkCanon(f.path());
	}
	ok(offCanon == 0, "permissions restored (canonical modes)",
		fmt("%d entries off canon, first: %s", offCanon, firstBad));

	printf("1..%d\n", testNum);
	return failures ? 1 : 0;
}
