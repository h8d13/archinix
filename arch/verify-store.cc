// Check that a store's contents still hash to what its db claims.
//
// The whole premise here is content addressing, and the store *is* the
// root filesystem, so silent corruption is a boot failure rather than a
// bad package. export-path already re-hashes on the way out for exactly
// that reason; this is the same check applied in place, before you find
// out at boot.
//
// Two levels, because the second is slow:
//   default   registration is consistent (paths in the db exist on
//             disk, referrers resolve)
//   --content also re-hash every store path and every .links entry
//             against the db's recorded NAR hash
//
// Detect only, never heal. Repair belonged to the substituter
// machinery, which this extraction dropped, and a verify that quietly
// dropped registrations would destroy the record you ran it to find:
// the basename is what a GRUB entry and a gcroot name. So a vanished
// path is reported, not pruned; prune it deliberately with rm-path,
// which drops the registration even when the directory is already
// gone. A corrupt generation is replaced by re-importing or by
// booting an older one.
//
// exit: 0 consistent, 1 usage/store error, 2 problems found
// usage: verify-store <store-root> [--content]
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <nix/store/globals.hh>
#include <nix/store/local-store.hh>
#include <nix/store/store-open.hh>

using namespace nix;

int main(int argc, char ** argv)
try {
	bool checkContents = false;
	const char * root = nullptr;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--content") == 0)
			checkContents = true;
		else if (!root)
			root = argv[i];
		else
			root = nullptr;
	}
	if (!root) {
		fprintf(stderr, "usage: %s <store-root> [--content]\n", argv[0]);
		return 1;
	}

	initLibStore();

	auto store = openStore(std::filesystem::absolute(root));
	auto local = store.dynamic_pointer_cast<LocalStore>();

	/* verifyStore reports through the logger and returns true when it
	   found something wrong; NoRepair because detect-only is the
	   contract above */
	bool errors = local->verifyStore(checkContents, NoRepair);

	if (errors) {
		fprintf(stderr, "store has problems (see above)\n");
		return 2;
	}
	printf("store is consistent%s\n", checkContents ? " (contents re-hashed)" : "");
	return 0;
} catch (std::exception & e) {
	fprintf(stderr, "verify-store: %s\n", e.what());
	return 1;
}
