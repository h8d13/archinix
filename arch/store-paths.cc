// List a local store's valid paths, one per line, as
// "<basename><TAB><registrationTime>" sorted by basename. The db is the
// store's source of truth: a directory listing also shows the .links
// dedup farm and half-written tmp-* imports, and can show a path that
// was never registered, so shell callers that globbed the store dir
// were filtering libnixstore's internals by hand.
// registrationTime is a unix timestamp, 0 when the db has none. It is
// the only "added at" a store path has: import canonicalises every
// mtime to 1, and ctime tracks later link-farm churn instead.
// Layout stays in the library: callers pass a store root and never
// spell out nix/store or nix/var/nix/db.
// usage: store-paths <store-root>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <nix/store/globals.hh>
#include <nix/store/local-store.hh>
#include <nix/store/store-open.hh>

using namespace nix;

int main(int argc, char ** argv)
try {
	if (argc != 2) {
		fprintf(stderr, "usage: %s <store-root>\n", argv[0]);
		return 1;
	}

	initLibStore();
	verbosity = lvlError;

	auto store = openStore(std::filesystem::absolute(argv[1]), true);

	std::vector<std::pair<std::string, time_t>> rows;
	for (auto & path : store->queryAllValidPaths())
		rows.emplace_back(std::string(path.to_string()),
			store->queryPathInfo(path)->registrationTime);

	/* sorted here so callers do not have to: store paths come out of
	   the db in id order, which is arrival order, not name order */
	std::sort(rows.begin(), rows.end());
	for (auto & [name, when] : rows)
		printf("%s\t%lld\n", name.c_str(), (long long) when);
	return 0;
} catch (std::exception & e) {
	fprintf(stderr, "store-paths: %s\n", e.what());
	return 1;
}
