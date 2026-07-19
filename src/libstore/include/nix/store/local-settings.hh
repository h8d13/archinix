#pragma once
///@file

#include "nix/util/types.hh"
#include "nix/util/configuration.hh"
#include "nix/util/experimental-features.hh"
#include "nix/util/users.hh"

#include "nix/store/config.hh"

#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace nix {

struct GCSettings : public virtual Config
{
private:
    void anchor() override;

public:
    Setting<off_t> reservedSize{
        this,
        8 * 1024 * 1024,
        "gc-reserved-space",
        "Amount of reserved disk space for the garbage collector.",
    };

    Setting<bool> keepOutputs{
        this,
        false,
        "keep-outputs",
        R"(
          If `true`, the garbage collector keeps the outputs of
          non-garbage derivations. If `false` (default), outputs are
          deleted unless they are GC roots themselves (or reachable from other
          roots).

          In general, outputs must be registered as roots separately. However,
          even if the output of a derivation is registered as a root, the
          collector still deletes store paths that are used only at build
          time (e.g., the C compiler, or source tarballs downloaded from the
          network). To prevent it from doing so, set this option to `true`.

          This option only applies to garbage collection of the whole store
          and does not affect deleting explicit paths.
        )",
        {"gc-keep-outputs"},
    };

    Setting<bool> keepDerivations{
        this,
        true,
        "keep-derivations",
        R"(
          If `true` (default), the garbage collector keeps the derivations
          from which non-garbage store paths were built. If `false`, they are
          deleted unless explicitly registered as a root (or reachable from
          other roots).

          Keeping derivation around is useful for querying and traceability
          (e.g., it allows you to ask with what dependencies or options a
          store path was built), so by default this option is on. Turn it off
          to save a bit of disk space (or a lot if `keep-outputs` is also
          turned on).

          This option only applies to garbage collection of the whole store
          and does not affect deleting explicit paths.
        )",
        {"gc-keep-derivations"},
    };

    Setting<uint64_t> minFree{
        this,
        0,
        "min-free",
        R"(
          When free disk space in `/nix/store` drops below `min-free` during a
          build, Nix performs a garbage-collection until `max-free` bytes are
          available or there is no more garbage. A value of `0` (the default)
          disables this feature.
        )",
    };

    // n.b. this is deliberately int64 max rather than uint64 max because
    // this goes through the Nix language JSON parser and thus needs to be
    // representable in Nix language integers.
    Setting<uint64_t> maxFree{
        this,
        std::numeric_limits<int64_t>::max(),
        "max-free",
        R"(
          When a garbage collection is triggered by the `min-free` option, it
          stops as soon as `max-free` bytes are available. The default is
          infinity (i.e. delete all garbage).
        )",
    };

    Setting<uint64_t> minFreeCheckInterval{
        this,
        5,
        "min-free-check-interval",
        "Number of seconds between checking free disk space.",
    };
};

/**
 * Either about local store or local building
 *
 * These are things that should not be part of the global settings, but
 * should be per-local-store at a minimum. We expose them from
 * `settings` with `settings.getLocalSettings()` for now, but we also
 * have `localStore.config->getLocalSettings()` as a way to get them
 * too. Even though both ways will actually draw from the same global
 * variable, we would much prefer if you use the second one, because
 * this will prepare the code base to making these *actual*, rather than
 * pretend, per-store settings.
 */
struct LocalSettings : public virtual Config, public GCSettings
{
private:
    void anchor() override;

public:
    /**
     * Get the GC settings.
     */
    GCSettings & getGCSettings()
    {
        return *this;
    }

    const GCSettings & getGCSettings() const
    {
        return *this;
    }

    Setting<bool> fsyncMetadata{
        this,
        true,
        "fsync-metadata",
        R"(
          If set to `true`, changes to the Nix store metadata (in
          `/nix/var/nix/db`) are synchronously flushed to disk. This improves
          robustness in case of system crashes, but reduces performance. The
          default is `true`.
        )"};

    Setting<bool> fsyncStorePaths{
        this,
        false,
        "fsync-store-paths",
        R"(
          Whether to call `fsync()` on store paths before registering them, to
          flush them to disk. This improves robustness in case of system crashes,
          but reduces performance. The default is `false`.
        )"};

    // FIXME: remove this option, `fsync-store-paths` is faster.
    Setting<bool> syncBeforeRegistering{
        this, false, "sync-before-registering", "Whether to call `sync()` before registering a path as valid."};

    Setting<bool> autoOptimiseStore{
        this,
        false,
        "auto-optimise-store",
        R"(
          If set to `true`, Nix automatically detects files in the store
          that have identical contents, and replaces them with hard links to
          a single copy. This saves disk space. If set to `false` (the
          default), you can still run `nix-store --optimise` to get rid of
          duplicate files.
        )"};

    Setting<size_t> narBufferSize{
        this, 32 * 1024 * 1024, "nar-buffer-size", "Maximum size of NARs before spilling them to disk."};

    Setting<bool> allowSymlinkedStore{
        this,
        false,
        "allow-symlinked-store",
        R"(
          If set to `true`, Nix stops complaining if the store directory
          (typically `/nix/store`) contains symlink components.

          This risks making some builds "impure" because builders sometimes
          "canonicalise" paths by resolving all symlink components. Problems
          occur if those builds are then deployed to machines where /nix/store
          resolves to a different location from that of the build machine. You
          can enable this setting if you are sure you're not going to do that.
        )"};

    Setting<std::string> buildUsersGroup{
        this,
        "",
        "build-users-group",
        R"(
          This options specifies the Unix group containing the Nix build user
          accounts. In multi-user Nix installations, builds should not be
          performed by the Nix account since that would allow users to
          arbitrarily modify the Nix store and database by supplying specially
          crafted builders; and they cannot be performed by the calling user
          since that would allow him/her to influence the build result.

          Therefore, if this option is non-empty and specifies a valid group,
          builds are performed under the user accounts that are a member
          of the group specified here (as listed in `/etc/group`). Those user
          accounts should not be used for any other purpose\!

          Nix never runs two builds under the same user account at the
          same time. This is to prevent an obvious security hole: a malicious
          user writing a Nix expression that modifies the build result of a
          legitimate Nix expression being built by another user. Therefore it
          is good to have as many Nix build user accounts as you can spare.
          (Remember: uids are cheap.)

          The build users should have permission to create files in the Nix
          store, but not delete them. Therefore, `/nix/store` should be owned
          by the Nix account, its group should be the group specified here,
          and its mode should be `1775`.

          If the build users group is empty, builds are performed under
          the uid of the Nix process (that is, the uid of the caller if
          `NIX_REMOTE` is empty, the uid under which the Nix daemon runs if
          `NIX_REMOTE` is `daemon`). Obviously, this should not be used
          with a nix daemon accessible to untrusted clients.

          Defaults to `nixbld` when running as root, *empty* otherwise.
        )",
        {},
        false};

    Setting<std::optional<AbsolutePath>> buildDir{
        this,
        std::nullopt,
        "build-dir",
        R"(
            Override the `build-dir` store setting for all stores that have this setting.

            See also the per-store [`build-dir`](@docroot@/store/types/local-store.md#store-local-store-build-dir) setting.
        )"};


private:

public:

#if NIX_SUPPORT_ACL
    Setting<StringSet> ignoredAcls{
        this,
        {"security.selinux", "system.nfs4_acl", "security.csm"},
        "ignored-acls",
        R"(
          A list of ACLs that should be ignored, normally Nix attempts to
          remove all ACLs from files and directories in the Nix store, but
          some ACLs like `security.selinux` or `system.nfs4_acl` can't be
          removed even by root. Therefore it's best to just ignore them.
        )"};
#endif

};

} // namespace nix
