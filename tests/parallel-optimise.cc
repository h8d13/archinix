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

	initLibStore(false);
	verbosity = lvlError;

	auto store = openStore("local?root=" + root);
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
		store->addToStore(fmt("t%d", p), {acc, CanonPath::root});
	}

	auto expected = walkStore(storeDir);
	ok(expected.size() == 40 * 51, "populated store",
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

	/* Full dedup: every content group collapsed to one inode. */
	std::map<std::string, std::set<ino_t>> groups;
	bool statFailed = false;
	for (auto & [p, content] : after) {
		struct stat st;
		if (::lstat(p.c_str(), &st) != 0)
			statFailed = true;
		else
			groups[content].insert(st.st_ino);
	}
	unsigned multi = 0;
	for (auto & [content, inos] : groups)
		if (inos.size() > 1)
			multi++;
	ok(!statFailed && multi == 0, "full dedup: one inode per distinct content",
		fmt("%d groups with >1 inode", multi));

	/* Permissions restored: nothing writable below store paths. */
	unsigned writable = 0;
	for (auto & ent : fs::directory_iterator(storeDir)) {
		auto name = ent.path().filename().string();
		if (name == ".links" || name.starts_with(".tmp-link"))
			continue;
		struct stat st;
		if (::lstat(ent.path().c_str(), &st) == 0 && (st.st_mode & S_IWUSR))
			writable++;
		if (ent.is_directory())
			for (auto & f : fs::recursive_directory_iterator(ent.path()))
				if (::lstat(f.path().c_str(), &st) == 0 && !S_ISLNK(st.st_mode)
					&& (st.st_mode & S_IWUSR))
					writable++;
	}
	ok(writable == 0, "permissions restored (nothing user-writable)",
		fmt("%d writable entries", writable));

	printf("1..%d\n", testNum);
	return failures ? 1 : 0;
}
