#include "nix/store/path-info.hh"
#include "nix/store/store-api.hh"
#include "nix/util/comparator.hh"
#include "nix/util/strings.hh"

namespace nix {

void UnkeyedValidPathInfo::anchor() {}

void ValidPathInfo::anchor() {}

UnkeyedValidPathInfo::UnkeyedValidPathInfo(const StoreDirConfig & store, Hash narHash)
    : UnkeyedValidPathInfo{store.storeDir, narHash}
{
}

GENERATE_CMP_EXT(
    ,
    std::weak_ordering,
    UnkeyedValidPathInfo,
    me->storeDir,
    me->deriver,
    me->narHash,
    me->references,
    me->registrationTime,
    me->narSize,
    // me->id,
    me->ultimate,
    me->ca);

std::optional<ContentAddressWithReferences> ValidPathInfo::contentAddressWithReferences() const
{
    if (!ca)
        return std::nullopt;

    switch (ca->method.raw) {
    case ContentAddressMethod::Raw::Text: {
        assert(references.count(path) == 0);
        return TextInfo{
            .hash = ca->hash,
            .references = references,
        };
    }

    case ContentAddressMethod::Raw::Flat:
    case ContentAddressMethod::Raw::NixArchive:
    default: {
        auto refs = references;
        bool hasSelfReference = false;
        if (refs.count(path)) {
            hasSelfReference = true;
            refs.erase(path);
        }
        return FixedOutputInfo{
            .method = ca->method.getFileIngestionMethod(),
            .hash = ca->hash,
            .references =
                {
                    .others = std::move(refs),
                    .self = hasSelfReference,
                },
        };
    }
    }
}

bool ValidPathInfo::isContentAddressed(const StoreDirConfig & store) const
{
    auto fullCaOpt = contentAddressWithReferences();

    if (!fullCaOpt)
        return false;

    auto caPath = store.makeFixedOutputPathFromCA(path.name(), *fullCaOpt);

    bool res = caPath == path;

    if (!res)
        printError("warning: path '%s' claims to be content-addressed but isn't", store.printStorePath(path));

    return res;
}

ValidPathInfo ValidPathInfo::makeFromCA(
    const StoreDirConfig & store, std::string_view name, ContentAddressWithReferences && ca, Hash narHash)
{
    ValidPathInfo res{
        store.makeFixedOutputPathFromCA(name, ca),
        UnkeyedValidPathInfo(store, narHash),
    };
    res.ca = ContentAddress{
        .method = ca.getMethod(),
        .hash = ca.getHash(),
    };
    res.references = std::visit(
        overloaded{
            [&](TextInfo && ti) { return std::move(ti.references); },
            [&](FixedOutputInfo && foi) {
                auto references = std::move(foi.references.others);
                if (foi.references.self)
                    references.insert(res.path);
                return references;
            },
        },
        std::move(ca).raw);
    return res;
}

} // namespace nix

