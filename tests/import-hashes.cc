// Regression test for per-file hash capture (ImportFileHashes).
// AsyncFileHasher re-implements the single-file NAR framing; if it ever
// drifts from what hashPath() computes from disk, optimisePath() farms
// files under wrong keys and dedup silently degrades. Pin it: every
// captured hash must equal a from-disk rehash of the restored file,
// capture must cover exactly the regular files (symlinks/dirs absent),
// and a capture-driven optimise must link duplicates and leave empty
// files alone.
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
#include <nix/util/file-content-address.hh>
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
static StorePath importTree(std::shared_ptr<LocalStore> local,
	ref<MemorySourceAccessor> acc, std::string_view name,
	LocalStore::ImportFileHashes & fileHashes)
{
	std::optional<StorePath> imported;
	auto sink = sourceToSink([&](Source & source) {
		imported = local->addToStoreFromDump(source, name,
			FileSerialisationMethod::NixArchive,
			ContentAddressMethod::Raw::NixArchive,
			HashAlgorithm::SHA256, {}, NoRepair, &fileHashes);
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

	initLibStore(false);
	verbosity = lvlError;

	auto store = openStore("local?root=" + fs::absolute(argv[1]).string());
	auto local = store.dynamic_pointer_cast<LocalStore>();
	fs::path linksDir = fs::path(argv[1]) / "nix/store/.links";

	using File = MemorySourceAccessor::File;
	std::string contentA(300, 'a'), contentB(300, 'b');

	/* every shape the capture must handle: nested regulars, an
	   executable, empty files, an in-tree duplicate, a symlink */
	auto acc = make_ref<MemorySourceAccessor>();
	acc->addFile(CanonPath("top"), std::string(contentA));
	acc->addFile(CanonPath("d1/d2/deep"), std::string(contentA)); // dup of top
	acc->addFile(CanonPath("d1/unique"), std::string(contentB));
	acc->open(CanonPath("bin/tool"),
		File{File::Regular{.executable = true, .contents = std::string(contentB)}});
	acc->addFile(CanonPath("empty1"), "");
	acc->addFile(CanonPath("d1/empty2"), "");
	acc->open(CanonPath("link"), File{File::Symlink{.target = "top"}});

	LocalStore::ImportFileHashes fileHashes;
	auto path = importTree(local, acc, "hashtest", fileHashes);
	auto realPath = local->toRealPath(path);

	/* capture covers exactly the regular files */
	std::set<std::string> expectedKeys{
		"/top", "/d1/d2/deep", "/d1/unique", "/bin/tool", "/empty1", "/d1/empty2"};
	std::set<std::string> gotKeys;
	for (auto & [k, h] : fileHashes.files)
		gotKeys.insert(k);
	ok(gotKeys == expectedKeys, "capture keys are exactly the regular files",
		fmt("got %d keys", gotKeys.size()));

	/* the drift guard: captured hash == from-disk rehash, per file */
	unsigned mismatches = 0;
	for (auto & [rel, captured] : fileHashes.files) {
		auto onDisk = hashPath(makeFSSourceAccessor(realPath + rel),
			FileSerialisationMethod::NixArchive, HashAlgorithm::SHA256).hash;
		if (captured != onDisk) {
			fprintf(stderr, "MISMATCH %s: captured %s, disk %s\n", rel.c_str(),
				captured.to_string(HashFormat::Nix32, true).c_str(),
				onDisk.to_string(HashFormat::Nix32, true).c_str());
			mismatches++;
		}
	}
	ok(mismatches == 0, "captured hashes equal from-disk NAR hashes",
		fmt("%d mismatches", mismatches));

	/* capture-driven optimise: the one in-tree duplicate gets linked
	   (first occurrence becomes the farm copy, not counted) */
	OptimiseStats stats;
	local->optimisePath(path, stats, &fileHashes);
	ok(stats.filesLinked == 1, "optimise links the in-tree duplicate",
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
	auto path2 = importTree(local, acc2, "hashtest2", fileHashes2);
	OptimiseStats stats2;
	local->optimisePath(path2, stats2, &fileHashes2);
	ok(fileHashes2.dedupedFiles == 1 && stats2.filesLinked == 0,
		"second import deduped against the farm while streaming",
		fmt("deduped %d, linked %d", fileHashes2.dedupedFiles,
			stats2.filesLinked));

	struct stat stTop, stAgain;
	ok(::lstat((realPath + "/top").c_str(), &stTop) == 0
			&& ::lstat((local->toRealPath(path2) + "/again").c_str(), &stAgain) == 0
			&& stTop.st_ino == stAgain.st_ino,
		"shared content collapsed to one inode across paths");

	printf("1..%d\n", testNum);
	return failures ? 1 : 0;
}
