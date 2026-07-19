#include <nlohmann/json.hpp>
#include <ranges>

#include "nix/util/base-n.hh"
#include "nix/util/signature.hh"
#include "nix/util/json-utils.hh"
#include "nix/util/util.hh"

namespace nix {

Signature Signature::parse(std::string_view s)
{
    size_t colon = s.find(':');
    if (colon == std::string::npos || colon == 0)
        throw FormatError("signature is corrupt");

    auto keyName = std::string(s.substr(0, colon));
    auto sig = base64::decode(s.substr(colon + 1));

    if (keyName.empty() || sig.empty())
        throw FormatError("signature is corrupt");

    return Signature{
        .keyName = std::move(keyName),
        .sig = std::move(sig),
    };
}

std::string Signature::to_string() const
{
    return keyName + ":" + base64::encode(std::as_bytes(std::span<const char>{sig.data(), sig.size()}));
}

template<typename Container>
std::set<Signature> Signature::parseMany(const Container & sigStrs)
{
    auto parsed = sigStrs | std::views::transform([](const auto & s) { return Signature::parse(s); });
    return std::set<Signature>(parsed.begin(), parsed.end());
}

template std::set<Signature> Signature::parseMany(const Strings &);
template std::set<Signature> Signature::parseMany(const StringSet &);

Strings Signature::toStrings(const std::set<Signature> & sigs)
{
    Strings res;
    for (const auto & sig : sigs) {
        res.push_back(sig.to_string());
    }

    return res;
}

} // namespace nix

namespace nlohmann {
void adl_serializer<Signature>::to_json(json & j, const Signature & s)
{
    j = {
        {"keyName", s.keyName},
        {"sig", base64::encode(std::as_bytes(std::span<const char>{s.sig}))},
    };
}

Signature adl_serializer<Signature>::from_json(const json & j)
{
    if (j.is_string())
        return Signature::parse(getString(j));
    auto obj = getObject(j);
    return Signature{
        .keyName = getString(valueAt(obj, "keyName")),
        .sig = base64::decode(getString(valueAt(obj, "sig"))),
    };
}

} // namespace nlohmann
