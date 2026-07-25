// Export store paths as a signed-format closure stream (nix-store
// --export wire format) to stdout: the "ship a generation to another
// machine" primitive. Every path is re-hashed against the store db
// before leaving, so local corruption cannot spread to the receiver.
// stdout is the binary stream; diagnostics go to stderr.
// usage: export-path <store-root> <store-path-basename>... > bundle
#include <cstdio>
#include <filesystem>
#include <unistd.h>

#include <nix/store/export-import.hh>
#include <nix/store/globals.hh>
#include <nix/store/store-open.hh>
#include <nix/util/serialise.hh>

using namespace nix;

int main(int argc, char ** argv)
try {
	if (argc < 3) {
		fprintf(stderr, "usage: %s <store-root> <store-path-basename>... > bundle\n", argv[0]);
		return 1;
	}
	if (isatty(STDOUT_FILENO)) {
		fprintf(stderr, "stdout is a terminal; redirect the stream (> bundle or | ssh ...)\n");
		return 1;
	}

	initLibStore();
	verbosity = lvlError;

	auto store = openStore(std::filesystem::absolute(argv[1]));

	StorePathSet paths;
	for (int i = 2; i < argc; i++)
		paths.insert(StorePath(argv[i]));

	FdSink out(STDOUT_FILENO);
	try {
		exportPaths(*store, paths, out);
		out.flush();
	} catch (Error & e) {
		/* e.g. on-disk content no longer matches the db hash */
		fprintf(stderr, "%s\n", e.what());
		return 1;
	}
	fprintf(stderr, "exported %zu paths, %.1f MiB\n",
		paths.size(), out.written / (1024.0 * 1024.0));
	return 0;
} catch (std::exception & e) {
	/* a bad basename, an unreadable store or a full disk must not
	   end in std::terminate: callers get a message and rc 1 */
	fprintf(stderr, "export-path: %s\n", e.what());
	return 1;
}
