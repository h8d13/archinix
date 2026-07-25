#include <string>

#include "nix/util/strings-inline.hh"
#include "nix/util/error.hh"
#include "nix/util/util.hh"

namespace nix {

template std::list<std::string> tokenizeString(std::string_view s, std::string_view separators);
template StringSet tokenizeString(std::string_view s, std::string_view separators);
template std::vector<std::string> tokenizeString(std::string_view s, std::string_view separators);

template std::list<std::string> splitString(std::string_view s, std::string_view separators);
template StringSet splitString(std::string_view s, std::string_view separators);
template std::vector<std::string> splitString(std::string_view s, std::string_view separators);

template std::list<OsString>
basicSplitString(std::basic_string_view<OsChar> s, std::basic_string_view<OsChar> separators);

template std::string concatStringsSep(std::string_view, const std::list<std::string> &);
template std::string concatStringsSep(std::string_view, const StringSet &);
template std::string concatStringsSep(std::string_view, const std::vector<std::string> &);
template std::string concatStringsSep(std::string_view, const boost::container::small_vector<std::string, 64> &);

typedef std::string_view strings_2[2];
template std::string concatStringsSep(std::string_view, const strings_2 &);
typedef std::string_view strings_3[3];
template std::string concatStringsSep(std::string_view, const strings_3 &);
typedef std::string_view strings_4[4];
template std::string concatStringsSep(std::string_view, const strings_4 &);

template std::string dropEmptyInitThenConcatStringsSep(std::string_view, const std::list<std::string> &);
template std::string dropEmptyInitThenConcatStringsSep(std::string_view, const StringSet &);
template std::string dropEmptyInitThenConcatStringsSep(std::string_view, const std::vector<std::string> &);

std::string optionalBracket(std::string_view prefix, std::string_view content, std::string_view suffix)
{
    if (content.empty()) {
        return "";
    }
    std::string result;
    result.reserve(prefix.size() + content.size() + suffix.size());
    result.append(prefix);
    result.append(content);
    result.append(suffix);
    return result;
}

const char * requireCString(const std::string & s)
{
    if (std::memchr(s.data(), '\0', s.size())) [[unlikely]] {
        using namespace std::string_view_literals;
        auto str = replaceStrings(s, "\0"sv, "␀"sv);
        throw Error("string '%s' with null (\\0) bytes used where it's not allowed", str);
    }
    return s.c_str();
}

} // namespace nix
