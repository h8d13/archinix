#include "nix/store/globals.hh"
#include "nix/store/store-open.hh"
#include "nix/util/topo-sort.hh"

namespace nix {

StorePaths Store::topoSortPaths(const StorePathSet & paths)
{
    auto result = topoSort(paths, [&](const StorePath & path) {
        try {
            return queryPathInfo(path)->references;
        } catch (InvalidPath &) {
            return StorePathSet();
        }
    });

    return std::visit(
        overloaded{
            /* plain Error, not BuildError: the build machinery is cut
               from this extraction */
            [&](const Cycle<StorePath> & cycle) -> StorePaths {
                throw Error(
                    "cycle detected in the references of '%s' from '%s'",
                    printStorePath(cycle.path),
                    printStorePath(cycle.parent));
            },
            [](const auto & sorted) { return sorted; }},
        result);
}

} // namespace nix
