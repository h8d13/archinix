// Resolve a generation token to its store basename, using the db.
// Accepts, in order of precedence: the full basename, a hash-part
// prefix of any length (nixgen-listid prints 8 chars), or the bare
// <name> given at import. Prints the resolved basename, or fails with a distinct exit
// code so callers can tell "no match" from "ambiguous".
//
// This exists because nixgen-remove, nixgen-switch and nixgen-diffid
// each carried their own copy of the same glob:
//
//   for m in "$S"/*-"$1"; do ...   # bare name
//   for m in "$S/$1"*; do ...      # hash prefix
//
// which is store-layout knowledge in the shell, matching against the
// same .links farm and half-written tmp-* imports that store-paths was
// written to stop exposing. The db is the source of truth for what is
// valid; a directory entry is not.
//
// exit: 0 resolved, 1 usage/store error, 2 no match, 3 ambiguous
// usage: store-resolve <store-root> <token>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <nix/store/globals.hh>
#include <nix/store/local-store.hh>
#include <nix/store/path.hh>
#include <nix/store/store-open.hh>

using namespace nix;

int main(int argc, char ** argv)
try {
	if (argc != 3) {
		fprintf(stderr, "usage: %s <store-root> <token>\n", argv[0]);
		return 1;
	}
	const std::string token = argv[2];

	initLibStore();
	verbosity = lvlError;

	auto store = openStore(std::filesystem::absolute(argv[1]), true);

	/* 1. already a full basename? cheapest check, and it is what the
	   other two forms resolve *to*, so try it first */
	try {
		StorePath exact{token};
		if (store->isValidPath(exact)) {
			printf("%s\n", std::string(exact.to_string()).c_str());
			return 0;
		}
	} catch (BadStorePath &) {
		/* not a basename; fall through to the other forms */
	}

	/* 2. hash-part prefix, resolved by the db rather than by globbing.
	   Any length up to the full 32 chars: nixgen-listid prints 8, and
	   the whole point of an id is that you can type it. */
	if (token.size() <= StorePath::HashLen) {
		auto hits = store->queryPathsByHashPrefix(token);
		if (hits.size() == 1) {
			printf("%s\n", std::string(hits.begin()->to_string()).c_str());
			return 0;
		}
		if (hits.size() > 1) {
			fprintf(stderr, "ambiguous id '%s' (%zu matches), use a longer id or the full basename:\n",
				token.c_str(), hits.size());
			for (auto & h : hits)
				fprintf(stderr, "  %s\n", std::string(h.to_string()).c_str());
			return 3;
		}
	}

	/* 3. bare <name>: the tail after the hash and its separator. The db
	   has no index for this, so scan the valid set: it is small (one
	   row per generation) and correctness beats a directory glob. */
	std::vector<std::string> matches;
	for (auto & path : store->queryAllValidPaths())
		if (path.name() == token)
			matches.push_back(std::string(path.to_string()));

	if (matches.size() == 1) {
		printf("%s\n", matches[0].c_str());
		return 0;
	}
	if (matches.size() > 1) {
		fprintf(stderr, "ambiguous name '%s' (%zu matches), use the full basename:\n",
			token.c_str(), matches.size());
		for (auto & m : matches)
			fprintf(stderr, "  %s\n", m.c_str());
		return 3;
	}

	fprintf(stderr, "no generation matches '%s'\n", token.c_str());
	return 2;
} catch (std::exception & e) {
	fprintf(stderr, "store-resolve: %s\n", e.what());
	return 1;
}
