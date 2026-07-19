#include "nix/store/globals.hh"
#include "nix/store/store-open.hh"
#include "nix/util/topo-sort.hh"
#include "nix/util/callback.hh"
#include "nix/util/closure.hh"

namespace nix {

void Store::computeFSClosure(
    const StorePathSet & startPaths,
    StorePathSet & paths_,
    bool flipDirection,
    bool includeOutputs,
    bool includeDerivers)
{
    std::function<asio::awaitable<StorePathSet>(const StorePath & path)> queryDeps;
    if (flipDirection)
        queryDeps = [this, includeOutputs, includeDerivers](const StorePath & path) -> asio::awaitable<StorePathSet> {
            StorePathSet res;
            StorePathSet referrers;
            queryReferrers(path, referrers);
            for (auto & ref : referrers)
                if (ref != path)
                    res.insert(ref);

            if (includeOutputs)
                for (auto & i : queryValidDerivers(path))
                    res.insert(i);
            co_return res;
        };
    else
        queryDeps = [this, includeOutputs, includeDerivers](const StorePath & path) -> asio::awaitable<StorePathSet> {
            StorePathSet res;
            auto info = co_await callbackToAwaitable<ref<const ValidPathInfo>>(
                [this, path](Callback<ref<const ValidPathInfo>> cb) { queryPathInfo(path, std::move(cb)); });

            for (auto & ref : info->references)
                if (ref != path)
                    res.insert(ref);

            if (includeDerivers && info->deriver && isValidPath(*info->deriver))
                res.insert(*info->deriver);
            co_return res;
        };

    computeClosure<StorePath>(startPaths, paths_, GetEdgesAsync<StorePath>(queryDeps));
}

void Store::computeFSClosure(
    const StorePath & startPath, StorePathSet & paths_, bool flipDirection, bool includeOutputs, bool includeDerivers)
{
    StorePathSet paths;
    paths.insert(startPath);
    computeFSClosure(paths, paths_, flipDirection, includeOutputs, includeDerivers);
}

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
