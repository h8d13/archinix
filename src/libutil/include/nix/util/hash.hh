#pragma once
///@file

#include "nix/util/types.hh"
#include "nix/util/serialise.hh"
#include "nix/util/file-system.hh"

namespace nix {

MakeError(BadHash, Error);

/**
 * SHA-256 is the only hash this store computes: store paths, the `ca`
 * assertion, the NAR hash in the db and the link farm's keys are all
 * it, and there is no place left where a caller could name another
 * (the fixed-output derivations that could are gone). It stays spelled
 * out in the rendered forms below, so what is already written on disk
 * still reads back.
 */
inline constexpr std::string_view hashAlgoName = "sha256";
inline constexpr size_t hashSizeSha256 = 32;

/**
 * @brief Enumeration representing the hash formats.
 */
enum struct HashFormat : int {
    /// @brief Nix-specific base-32 encoding. @see BaseNix32
    Nix32,
    /// @brief Lowercase hexadecimal encoding. @see base16Chars
    Base16,
    /// @brief "<hash algo>-<Base 64 hash>", format of the SRI integrity attribute.
    /// @see W3C recommendation [Subresource Integrity](https://www.w3.org/TR/SRI/).
    SRI
};

struct Hash
{
    /** Opaque handle type for the hash calculation state. */
    struct Ctx;

    constexpr static size_t maxHashSize = hashSizeSha256;

    /**
     * Not always the digest length: `compressHash` folds a digest down
     * to a shorter one for store path names.
     */
    size_t hashSize = hashSizeSha256;
    uint8_t hash[maxHashSize] = {};

    /**
     * A zero-filled hash, which is also how "hash not known" is
     * spelled in the db.
     */
    Hash() = default;

    /**
     * Parse a hash that carries its algorithm, as `sha256:<hash>` or
     * (SRI) `sha256-<hash>`. The base encoding follows from the
     * length.
     */
    static Hash parseAnyPrefixed(std::string_view s);

    /**
     * Check whether two hashes are equal.
     */
    bool operator==(const Hash & h2) const noexcept;

    /**
     * Return a string representation of the hash, in base-32, base-16
     * or SRI (`sha256-<base64>`). Prefixed by the hash algo
     * (e.g. "sha256:") unless `includeAlgo` says otherwise; SRI always
     * carries it. Bare base-64 was a fourth choice no caller made.
     */
    [[nodiscard]] std::string to_string(HashFormat hashFormat, bool includeAlgo) const;
};

/**
 * Compute the hash of the given string.
 */
Hash hashString(std::string_view s);

/**
 * The final hash and the number of bytes digested.
 */
struct HashResult
{
    Hash hash;
    uint64_t numBytesDigested;
};

/**
 * Compress a hash to the specified number of bytes by cyclically
 * XORing bytes together.
 */
Hash compressHash(const Hash & hash, unsigned int newSize);

class HashSink : public BufferedSink
{
private:
    Hash::Ctx * ctx;
    uint64_t bytes;

    void anchor() override;

public:
    HashSink();
    ~HashSink();
    void writeUnbuffered(std::string_view data) override;
    HashResult finish();
    HashResult currentHash();
};

} // namespace nix

template<>
struct std::hash<nix::Hash>
{
    std::size_t operator()(const nix::Hash & hash) const noexcept
    {
        assert(hash.hashSize > sizeof(size_t));
        return *reinterpret_cast<const std::size_t *>(&hash.hash);
    }
};

namespace nix {

inline std::size_t hash_value(const Hash & hash)
{
    return std::hash<Hash>{}(hash);
}

} // namespace nix
