#include "nix/util/url.hh"

namespace nix {

std::string percentEncode(std::string_view s, std::string_view keep)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string res;
    res.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.'
            || c == '_' || c == '~' || keep.find(c) != keep.npos)
            res += c;
        else {
            res += '%';
            res += hex[c >> 4];
            res += hex[c & 0xf];
        }
    }
    return res;
}

} // namespace nix
