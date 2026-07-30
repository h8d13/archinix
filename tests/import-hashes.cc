// Regression test for per-file hash capture (ImportFileHashes).
// AsyncFileHasher re-implements the single-file NAR framing; if it ever
// drifts from what hashPath() computes from disk, optimisePath() farms
// files under wrong keys and dedup silently degrades. Pin it: every
// captured hash must equal a from-disk rehash of the restored node,
// capture must cover exactly the regular files and symlinks (dirs
// absent), and a capture-driven optimise must link duplicates and
// leave empty files alone.
// Symlinks are in the map so optimisePath does not have to rehash them
// from disk, which costs an O_NOFOLLOW open that takes ELOOP plus a
// reopen of the parent and a readlink. Their framing is hand-written
// in AsyncFileHasher::symlink and the rehash below is what pins it.
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <sys/stat.h>

#include <nix/store/globals.hh>
#include <nix/store/local-store.hh>
#include <nix/store/store-open.hh>
#include <nix/util/archive.hh>
#include <nix/util/memory-source-accessor.hh>
#include <nix/util/serialise.hh>
#include <nix/util/source-accessor.hh>

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

// import an in-memory tree with per-file hash capture, import-dir style
static StorePath importTree(std::shared_ptr<LocalStore> store,
	ref<MemorySourceAccessor> acc, std::string_view name,
	LocalStore::ImportFileHashes & fileHashes)
{
	std::optional<StorePath> imported;
	auto sink = sourceToSink([&](Source & source) {
		imported = store->addToStoreFromDump(source, name, &fileHashes);
	});
	SourcePath{acc, CanonPath::root}.dumpPath(*sink);
	sink->finish();
	return *imported;
}

int main(int argc, char ** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <store-root>\n", argv[0]);
		return 1;
	}

	initLibStore();
	verbosity = lvlError;

	auto store = openStore(fs::absolute(argv[1]));
	fs::path linksDir = fs::path(argv[1]) / "nix/store/.links";

	using File = MemorySourceAccessor::File;
	std::string contentA(300, 'a'), contentB(300, 'b');

	/* every shape the capture must handle: nested regulars, an
	   executable, empty files, an in-tree duplicate, and symlinks --
	   a short target, a long one (past the 60-byte ext4 fast-symlink
	   boundary), a dangling one, and two that share a target so the
	   farm has to collapse them */
	auto acc = make_ref<MemorySourceAccessor>();
	acc->addFile(CanonPath("top"), std::string(contentA));
	acc->addFile(CanonPath("d1/d2/deep"), std::string(contentA)); // dup of top
	acc->addFile(CanonPath("d1/unique"), std::string(contentB));
	acc->open(CanonPath("bin/tool"),
		File{File::Regular{.executable = true, .contents = std::string(contentB)}});
	acc->addFile(CanonPath("empty1"), "");
	acc->addFile(CanonPath("d1/empty2"), "");
	acc->open(CanonPath("link"), File{File::Symlink{.target = "top"}});
	acc->open(CanonPath("d1/longlink"),
		File{File::Symlink{.target = std::string(200, 'x')}});
	acc->open(CanonPath("d1/dangling"),
		File{File::Symlink{.target = "/nowhere/at/all"}});
	acc->open(CanonPath("d1/samelink"), File{File::Symlink{.target = "top"}});

	LocalStore::ImportFileHashes fileHashes;
	auto path = importTree(store, acc, "hashtest", fileHashes);
	auto realPath = store->toRealPath(path);

	/* capture covers exactly the regular files and the symlinks */
	std::set<std::string> expectedKeys{
		"/top", "/d1/d2/deep", "/d1/unique", "/bin/tool", "/empty1", "/d1/empty2",
		"/link", "/d1/longlink", "/d1/dangling", "/d1/samelink"};
	std::set<std::string> gotKeys;
	for (auto & [k, h] : fileHashes.files)
		gotKeys.insert(k);
	ok(gotKeys == expectedKeys, "capture keys are the regular files and symlinks",
		fmt("got %d keys", gotKeys.size()));

	/* two symlinks with the same target must hash the same, a
	   different target must not: the framing has to actually depend on
	   the target and on nothing else */
	ok(fileHashes.files.at("/link") == fileHashes.files.at("/d1/samelink")
			&& fileHashes.files.at("/link") != fileHashes.files.at("/d1/longlink"),
		"symlink digests track the target");

	/* the drift guard: captured hash == from-disk rehash, per file */
	unsigned mismatches = 0;
	for (auto & [rel, captured] : fileHashes.files) {
		auto onDisk = hashPath(makeFSSourceAccessor(realPath + rel)).hash;
		if (captured != onDisk) {
			fprintf(stderr, "MISMATCH %s: captured %s, disk %s\n", rel.c_str(),
				captured.to_string(HashFormat::Nix32, true).c_str(),
				onDisk.to_string(HashFormat::Nix32, true).c_str());
			mismatches++;
		}
	}
	ok(mismatches == 0, "captured hashes equal from-disk NAR hashes",
		fmt("%d mismatches", mismatches));

	/* capture-driven optimise: the in-tree duplicates get linked (first
	   occurrence of each becomes the farm copy and is not counted), so
	   d1/d2/deep against top and d1/samelink against link. The symlink
	   half is what proves the captured symlink digests are usable as
	   farm keys and not merely equal to hashPath's. */
	OptimiseStats stats;
	store->optimisePath(path, stats, &fileHashes);
	ok(stats.filesLinked == 2, "optimise links the duplicate file and symlink",
		fmt("linked %d", stats.filesLinked));

	/* empty files stay unlinked: distinct inodes, no farm entry */
	struct stat st1, st2;
	ok(::lstat((realPath + "/empty1").c_str(), &st1) == 0
			&& ::lstat((realPath + "/d1/empty2").c_str(), &st2) == 0
			&& st1.st_nlink == 1 && st2.st_nlink == 1
			&& st1.st_ino != st2.st_ino,
		"empty files not welded (nlink 1, distinct inodes)");
	unsigned zeroLinks = 0;
	for (auto & ent : fs::directory_iterator(linksDir))
		if (ent.is_regular_file() && ent.file_size() == 0)
			zeroLinks++;
	ok(zeroLinks == 0, "link farm holds no empty entries",
		fmt("%d zero-size entries", zeroLinks));

	/* cross-path dedup through the capture: a second import sharing
	   contentA dedups against the farm entry made above while the
	   import streams (tryDedup), so optimise finds nothing left */
	auto acc2 = make_ref<MemorySourceAccessor>();
	acc2->addFile(CanonPath("again"), std::string(contentA));
	acc2->addFile(CanonPath("fresh"), std::string(300, 'c'));

	LocalStore::ImportFileHashes fileHashes2;
	auto path2 = importTree(store, acc2, "hashtest2", fileHashes2);
	OptimiseStats stats2;
	store->optimisePath(path2, stats2, &fileHashes2);
	ok(fileHashes2.dedupedFiles == 1 && stats2.filesLinked == 0,
		"second import deduped against the farm while streaming",
		fmt("deduped %d, linked %d", fileHashes2.dedupedFiles,
			stats2.filesLinked));

	struct stat stTop, stAgain;
	ok(::lstat((realPath + "/top").c_str(), &stTop) == 0
			&& ::lstat((store->toRealPath(path2) + "/again").c_str(), &stAgain) == 0
			&& stTop.st_ino == stAgain.st_ino,
		"shared content collapsed to one inode across paths");

	printf("1..%d\n", testNum);
	return failures ? 1 : 0;
}
