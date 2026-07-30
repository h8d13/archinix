#include <cstring>

#include <openssl/crypto.h>
#include <openssl/sha.h>

#include "nix/util/hash.hh"
#include "nix/util/split.hh"
#include "nix/util/base-n.hh"
#include "nix/util/base-nix-32.hh"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


namespace nix {

void BadHash::anchor() {}

void HashSink::anchor() {}

bool Hash::operator==(const Hash & h2) const noexcept
{
    if (hashSize != h2.hashSize)
        return false;
    for (unsigned int i = 0; i < hashSize; i++)
        if (hash[i] != h2.hash[i])
            return false;
    return true;
}

std::string Hash::to_string(HashFormat hashFormat, bool includeAlgo) const
{
    std::string s;
    if (hashFormat == HashFormat::SRI || includeAlgo) {
        s += hashAlgoName;
        s += hashFormat == HashFormat::SRI ? '-' : ':';
    }
    assert(hashSize);
    const auto bytes = std::as_bytes(std::span<const uint8_t>{&hash[0], hashSize});
    switch (hashFormat) {
    case HashFormat::Base16:
        s += base16::encode(bytes);
        break;
    case HashFormat::Nix32:
        s += BaseNix32::encode(bytes);
        break;
    case HashFormat::Base64:
    case HashFormat::SRI:
        s += base64::encode(bytes);
        break;
    }
    return s;
}

namespace {

/// Private convenience
struct DecodeNamePair
{
    decltype(base16::decode) * decode;
    std::string_view encodingName;
};

} // namespace

/**
 * Given the expected size of the message once decoded, figure out
 * which encoding we are using by looking at the size of the encoded
 * message.
 */
static DecodeNamePair baseFromSize(std::string_view rest)
{
    if (rest.size() == base16::encodedLength(hashSizeSha256))
        return {base16::decode, "base16"};

    if (rest.size() == BaseNix32::encodedLength(hashSizeSha256))
        return {BaseNix32::decode, "nix32"};

    if (rest.size() == base64::encodedLength(hashSizeSha256))
        return {base64::decode, "base64"};

    throw BadHash("hash '%s' has wrong length for hash algorithm '%s'", rest, hashAlgoName);
}

/**
 * @param rest the string view to parse. Must not include any
 * `sha256(:|-)` prefix.
 */
static Hash parseLowLevel(std::string_view rest, DecodeNamePair pair)
{
    Hash res;
    std::string d;
    try {
        d = pair.decode(rest);
    } catch (Error & e) {
        e.addTrace("While decoding hash '%s'", rest);
    }
    if (d.size() != res.hashSize)
        throw BadHash(
            "invalid %s hash '%s', length %d != expected length %d", pair.encodingName, rest, d.size(), res.hashSize);
    memcpy(res.hash, d.data(), res.hashSize);

    return res;
}

Hash Hash::parseAnyPrefixed(std::string_view original)
{
    auto rest = original;

    bool isSRI = false;
    auto algoRaw = splitPrefixTo(rest, ':');
    if (!algoRaw) {
        algoRaw = splitPrefixTo(rest, '-');
        if (algoRaw)
            isSRI = true;
    }

    if (!algoRaw)
        throw BadHash("hash '%s' does not include a type", original);
    if (*algoRaw != hashAlgoName)
        throw BadHash("hash '%s' is not %s, which is the only algorithm this store uses", original, hashAlgoName);

    /* SRI is always Base64; otherwise the length says which base. */
    return parseLowLevel(rest, isSRI ? DecodeNamePair{base64::decode, "SRI"} : baseFromSize(rest));
}

Hash Hash::parseNonSRIUnprefixed(std::string_view s)
{
    return parseLowLevel(s, baseFromSize(s));
}

struct Hash::Ctx
{
    SHA256_CTX sha256;
};

Hash hashString(std::string_view s)
{
    Hash::Ctx ctx;
    Hash hash;
    SHA256_Init(&ctx.sha256);
    SHA256_Update(&ctx.sha256, s.data(), s.size());
    SHA256_Final(hash.hash, &ctx.sha256);
    return hash;
}

/* 1 KiB, not BufferedSink's 32 KiB default: the only writes this
   buffer coalesces are the NAR framing tokens ("(", "type", "regular",
   length words), a few dozen bytes per file. Content arrives in chunks
   larger than any sane buffer and bypasses it either way, so a per-file
   hash sink was allocating and freeing 32 KiB to batch ~40 bytes. */
HashSink::HashSink()
    : BufferedSink(1024)
{
    ctx = new Hash::Ctx;
    bytes = 0;
    SHA256_Init(&ctx->sha256);
}

HashSink::~HashSink()
{
    bufPos = 0;
    delete ctx;
}

void HashSink::writeUnbuffered(std::string_view data)
{
    bytes += data.size();
    SHA256_Update(&ctx->sha256, data.data(), data.size());
}

HashResult HashSink::finish()
{
    flush();
    Hash hash;
    SHA256_Final(hash.hash, &ctx->sha256);
    return HashResult(hash, bytes);
}

HashResult HashSink::currentHash()
{
    flush();
    Hash::Ctx ctx2 = *ctx;
    Hash hash;
    SHA256_Final(hash.hash, &ctx2.sha256);
    return HashResult(hash, bytes);
}

Hash compressHash(const Hash & hash, unsigned int newSize)
{
    Hash h;
    h.hashSize = newSize;
    for (unsigned int i = 0; i < hash.hashSize; ++i)
        h.hash[i % newSize] ^= hash.hash[i];
    return h;
}

} // namespace nix
