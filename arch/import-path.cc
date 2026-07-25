// Import a closure stream produced by export-path from stdin, then
// hard-link-deduplicate each imported path against the link farm (no
// captured hashes here: the receive side re-reads, correctness over
// speed). Our stores are unsigned, so integrity is enforced the CA
// way: the received NAR is re-hashed and the store path is recomputed
// from that hash; a stream whose content does not hash back to the
// path it claims is rejected before registration. (Plain importPaths
// would accept it: framing-valid corruption keeps the claimed name.)
// Each imported path is registered as a GC root under
// <root>/nix/var/nix/gcroots/<basename>, as import-dir does: arriving
// content is live before it has a GRUB entry, and rm-path unroots
// before deleting.
// stdout is the imported real paths, one per line, import order;
// diagnostics go to stderr.
// usage: import-path <store-root> < bundle
#include <cstdio>
#include <filesystem>
#include <unistd.h>

#include <nix/store/export-import.hh>
#include <nix/store/globals.hh>
#include <nix/store/local-store.hh>
#include <nix/store/store-open.hh>
#include <nix/util/archive.hh>
#include <nix/util/fs-sink.hh>
#include <nix/util/serialise.hh>
#include <nix/store/common-protocol.hh>
#include <nix/store/common-protocol-impl.hh>

using namespace nix;

// export-import.cc's importPaths loop with the CA check inserted
// between hashing and registration
static StorePaths importVerified(Store & store, Source & source)
{
	StorePaths res;
	while (true) {
		auto n = readNum<uint64_t>(source);
		if (n == 0)
			break;
		if (n != 1)
			throw Error("input doesn't look like an export-path stream");

		StringSink saved;
		TeeSource tee{source, saved};
		NullFileSystemObjectSink ether;
		parseDump(ether, tee);

		if (readInt(source) != exportMagic)
			throw Error("Nix archive cannot be imported; wrong format");

		auto path = store.parseStorePath(readString(source));
		auto references = CommonProto::Serialise<StorePathSet>::read(
			store, CommonProto::ReadConn{.from = source});
		auto deriver = readString(source);
		auto narHash = hashString(HashAlgorithm::SHA256, saved.s);

		auto expected = store.makeFixedOutputPathFromCA(path.name(),
			ContentAddressWithReferences::withoutRefs(ContentAddress{
				.method = ContentAddressMethod::Raw::NixArchive,
				.hash = narHash}));
		if (path != expected)
			throw Error(
				"integrity check failed: stream claims '%s' but its content "
				"hashes to '%s' (corrupted or not a CA NixArchive path)",
				store.printStorePath(path), store.printStorePath(expected));

		/* Generations are independent, self-contained trees: import-dir
		   creates them with no references, and sharing between them is
		   the .links hardlink farm, which works on content below the
		   store-path level. This is the only door references could
		   enter through, so refuse them here rather than carry a
		   closure walk for a case that never occurs. */
		if (!references.empty())
			throw Error(
				"path '%s' claims %zu references; this store holds "
				"independent trees",
				store.printStorePath(path), references.size());

		ValidPathInfo info{path, {store, narHash}};
		info.references = references;
		info.narSize = saved.s.size();
		if (deriver != "")
			info.deriver = store.parseStorePath(deriver);

		// ignore optional legacy signature
		if (readInt(source) == 1)
			readString(source);

		StringSource body{saved.s};
		store.addToStore(info, body, NoRepair);
		res.push_back(path);
	}
	return res;
}

int main(int argc, char ** argv)
try {
	if (argc != 2) {
		fprintf(stderr, "usage: %s <store-root> < bundle\n", argv[0]);
		return 1;
	}
	if (isatty(STDIN_FILENO)) {
		fprintf(stderr, "stdin is a terminal; feed an export-path stream (< bundle)\n");
		return 1;
	}

	initLibStore();
	verbosity = lvlError;

	auto store = openStore(std::filesystem::absolute(argv[1]));
	auto local = store.dynamic_pointer_cast<LocalStore>();

	FdSource in(STDIN_FILENO);
	StorePaths paths;
	try {
		paths = importVerified(*store, in);
	} catch (Error & e) {
		fprintf(stderr, "%s\n", e.what());
		return 1;
	}

	/* Root before anything else can collect them: a received
	   generation is live from the moment it lands, whether or not it
	   ever gets a GRUB entry. nixgen-adopt hands out entries later and
	   nixgen-remove drops entry and root together. */
	for (auto & path : paths)
		local->addPermRoot(path,
			local->config->stateDir / "gcroots"
				/ std::string(path.to_string()));

	OptimiseStats stats;
	for (auto & path : paths)
		local->optimisePath(path, stats, nullptr);
	fprintf(stderr, "imported %zu paths; optimise: linked %lu files, freed %.1f MiB\n",
		paths.size(), stats.filesLinked, stats.bytesFreed / (1024.0 * 1024.0));

	for (auto & path : paths)
		printf("%s\n", local->toRealPath(path).c_str());
	return 0;
} catch (std::exception & e) {
	/* a bad basename, an unreadable store or a full disk must not
	   end in std::terminate: callers get a message and rc 1 */
	fprintf(stderr, "import-path: %s\n", e.what());
	return 1;
}
