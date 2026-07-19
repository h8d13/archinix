#include "nix/store/references.hh"
#include "nix/util/hash.hh"

#include <cstdlib>
#include <algorithm>

namespace nix {

void HashModuloSink::anchor() {}

void RewritingSink::anchor() {}

RewritingSink::RewritingSink(const std::string & from, const std::string & to, Sink & nextSink)
    : RewritingSink({{from, to}}, nextSink)
{
}

RewritingSink::RewritingSink(const StringMap & rewrites, Sink & nextSink)
    : rewrites(rewrites)
    , nextSink(nextSink)
{
    std::string::size_type maxRewriteSize = 0;
    for (auto & [from, to] : rewrites) {
        assert(from.size() == to.size());
        maxRewriteSize = std::max(maxRewriteSize, from.size());
    }
    this->maxRewriteSize = maxRewriteSize;
}

void RewritingSink::operator()(std::string_view data)
{
    std::string s(prev);
    s.append(data);

    s = rewriteStrings(s, rewrites, &matches, pos);

    prev = s.size() < maxRewriteSize ? s
           : maxRewriteSize == 0     ? ""
                                     : std::string(s, s.size() - maxRewriteSize + 1, maxRewriteSize - 1);

    auto consumed = s.size() - prev.size();

    pos += consumed;

    if (consumed)
        nextSink(s.substr(0, consumed));
}

void RewritingSink::flush()
{
    if (prev.empty())
        return;
    pos += prev.size();
    nextSink(prev);
    prev.clear();
}

HashModuloSink::HashModuloSink(HashAlgorithm ha, const std::string & modulus)
    : hashSink(ha)
    // Zero out self-references (the "modulus").
    , rewritingSink(modulus, std::string(modulus.size(), 0), hashSink)
{
}

void HashModuloSink::operator()(std::string_view data)
{
    rewritingSink(data);
}

HashResult HashModuloSink::finish()
{
    rewritingSink.flush();

    /* Hash the positions of the self-references. This ensures that a
       NAR with self-references and a NAR with some of the
       self-references already zeroed out do not produce a hash
       collision. FIXME: proof. */
    for (auto & pos : rewritingSink.matches)
        hashSink(fmt("|%d", pos));

    auto h = hashSink.finish();
    return {.hash = h.hash, .numBytesDigested = rewritingSink.pos};
}

} // namespace nix
