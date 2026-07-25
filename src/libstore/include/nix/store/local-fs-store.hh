#pragma once
///@file

#include "nix/store/store-api.hh"
#include "nix/store/gc-store.hh"

namespace nix {

struct LocalFSStoreConfig : virtual StoreConfig
{
private:
    void anchor() override;

public:
    /**
     * The store is opened with a root path and nothing else. `stateDir`
     * and `realStoreDir` are derived from it rather than being
     * separately settable: there is no config surface to set them
     * through, and arch/ owns the layout above this point.
     */
    LocalFSStoreConfig(const std::filesystem::path & rootDir);

    /**
     * Directory prefixed to all other paths.
     */
    std::filesystem::path rootDir;

    /**
     * Directory where the store keeps its state (`<root>/nix/var/nix`).
     */
    std::filesystem::path stateDir;

    /**
     * Physical path of the store (`<root>/nix/store`). The *logical*
     * store path stays `storeDir`; only the physical half moves with
     * the root.
     */
    std::filesystem::path realStoreDir;

    const std::filesystem::path & getStateDir() const override
    {
        return stateDir;
    }
};

struct alignas(8) /* Work around ASAN failures on i686-linux. */
    LocalFSStore : virtual Store,
                   virtual GcStore
{
private:
    void anchor() override;

public:
    using Config = LocalFSStoreConfig;

    const Config & config;

    inline static std::string operationName = "Local Filesystem Store";

    const static std::filesystem::path drvsLogDir;

    LocalFSStore(const Config & params);

    ref<SourceAccessor> getFSAccessor(bool requireValidPath = true) override;
    std::shared_ptr<SourceAccessor> getFSAccessor(const StorePath & path, bool requireValidPath = true) override;

    /**
     * Creates symlink from the `gcRoot` to the `storePath` and
     * registers the `gcRoot` as a permanent GC root. The `gcRoot`
     * symlink lives outside the store and is created and owned by the
     * user.
     *
     * @param gcRoot The location of the symlink.
     *
     * @param storePath The store object being rooted. The symlink will
     * point to `toRealPath(storePath)`.
     *
     * How the permanent GC root corresponding to this symlink is
     * managed is implementation-specific.
     */
    virtual std::filesystem::path addPermRoot(const StorePath & storePath, const std::filesystem::path & gcRoot) = 0;

    virtual std::filesystem::path getRealStoreDir()
    {
        return config.realStoreDir;
    }

    std::filesystem::path toRealPath(const StorePath & storePath)
    {
        return getRealStoreDir() / storePath.to_string();
    }
};

} // namespace nix
