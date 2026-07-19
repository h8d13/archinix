#pragma once
///@file

#include "nix/util/json-impls.hh"

#include <map>

namespace nix {

/**
 * A cryptographic signature along with the name of the key that produced it.
 *
 * Serialized as `<key-name>:<signature-in-Base64>`.
 *
 * Signing and verification machinery is gone (substituter trust was
 * signature-based; integrity here is CA re-hash on import). The type
 * survives so the `sigs` column of the SQLite schema round-trips
 * untouched.
 */
struct Signature
{
    std::string keyName;

    /**
     * The raw decoded signature bytes.
     */
    std::string sig;

    /**
     * Parse a signature in the format `<key-name>:<signature-in-Base64>`.
     */
    static Signature parse(std::string_view);

    /**
     * Parse multiple signatures from a container of strings.
     *
     * Each string must be in the format `<key-name>:<signature-in-Base64>`.
     */
    template<typename Container>
    static std::set<Signature> parseMany(const Container & sigStrs);

    std::string to_string() const;

    static Strings toStrings(const std::set<Signature> & sigs);

    auto operator<=>(const Signature &) const = default;
};

} // namespace nix

JSON_IMPL(nix::Signature)
