#pragma once
///@file

#include <sys/types.h>

#include "nix/util/types.hh"
#include "nix/util/configuration.hh"
#include "nix/util/environment-variables.hh"
#include "nix/store/local-settings.hh"
#include "nix/store/store-reference.hh"

#include "nix/store/config.hh"

namespace nix {

class Settings : public virtual Config, private LocalSettings
{
private:
    void anchor() override;
public:
public:

    Settings();

    /**
     * Get the local store settings.
     */
    LocalSettings & getLocalSettings()
    {
        return *this;
    }

    const LocalSettings & getLocalSettings() const
    {
        return *this;
    }




    /**
     * The directory where state is stored.
     */
    std::filesystem::path nixStateDir;

    Setting<StoreReference> storeUri{
        this,
        StoreReference::parse(getEnv("NIX_REMOTE").value_or("auto")),
        "store",
        R"(
          The [URL of the Nix store](@docroot@/store/types/index.md#store-url-format)
          to use for most operations.

          See the
          [Store Types](@docroot@/store/types/index.md)
          section of the manual for supported store types and settings.

          Can be overridden by the [`NIX_REMOTE`](@docroot@/command-ref/env-common.md#env-NIX_REMOTE) environment variable.

          The default value is [`auto`](@docroot@/store/types/index.md#auto).
        )"};

    /* always WAL: the isWSL1() carve-out went with the build machinery */
    Setting<bool> useSQLiteWAL{this, true, "use-sqlite-wal", "Whether SQLite should use WAL mode."};

    /**
     * Whether to show build log output in real time.
     */
    bool verboseBuild = true;

    /**
     * Read-only mode.  Don't copy stuff to the store, don't change
     * the database.
     */
    bool readOnlyMode = false;

    Setting<uint64_t> warnLargePathThreshold{
        this,
        0,
        "warn-large-path-threshold",
        R"(
          Warn when copying a path larger than this number of bytes to the Nix store
          (as determined by its NAR serialisation).
          Default is 0, which disables the warning.
          Set it to 1 to warn on all paths.
        )"};

    /**
     * Get the options needed for profile directory functions.
     */
};

// FIXME: don't use a global variable.
extern nix::Settings settings;

/**
 * Load the configuration (from `nix.conf`, `NIX_CONFIG`, etc.) into the
 * given configuration object.
 *
 * Usually called with `globalConfig`.
 */
void loadConfFile(AbstractConfig & config);

/**
 * The version of Nix itself.
 *
 * This is not `const`, so that the Nix CLI can provide a more detailed version
 * number including the git revision, without having to "re-compile" the entire
 * set of Nix libraries to include that version, even when those libraries are
 * not affected by the change.
 */
extern std::string nixVersion;

/**
 * @param loadConfig Whether to load configuration from `nix.conf`, `NIX_CONFIG`, etc. May be disabled for unit tests.
 * @note When using libexpr, and/or libmain, This is not sufficient. See initNix().
 */
void initLibStore(bool loadConfig = true);

/**
 * It's important to initialize before doing _anything_, which is why we
 * call upon the programmer to handle this correctly. However, we only add
 * this in a key locations, so as not to litter the code.
 */
void assertLibStoreInitialized();

} // namespace nix
