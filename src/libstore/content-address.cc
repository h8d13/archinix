#include "nix/store/content-address.hh"
#include "nix/util/split.hh"

namespace nix {

/* `fixed:r:` is the recursive-NAR spelling, kept verbatim so the `ca`
   column of an existing store still reads back. Nothing branches on
   it: a value that does not carry it was not written by this store. */
static constexpr std::string_view caPrefix = "fixed:r:";

std::string ContentAddress::render() const
{
    return std::string{caPrefix} + hash.to_string(HashFormat::Nix32, true);
}

ContentAddress ContentAddress::parse(std::string_view rawCa)
{
    auto rest = rawCa;

    if (!splitPrefix(rest, caPrefix))
        throw UsageError("content address '%s' is not a recursive NAR address ('%s')", rawCa, caPrefix);

    return ContentAddress{
        .hash = Hash::parseAnyPrefixed(rest),
    };
}

std::optional<ContentAddress> ContentAddress::parseOpt(std::string_view rawCaOpt)
{
    return rawCaOpt == "" ? std::nullopt : std::optional{ContentAddress::parse(rawCaOpt)};
};

std::string renderContentAddress(std::optional<ContentAddress> ca)
{
    return ca ? ca->render() : "";
}

} // namespace nix
