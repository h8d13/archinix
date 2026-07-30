#pragma once

#include "nix/util/types.hh"

#include <list>
#include <optional>
#include <set>
#include <string_view>
#include <string>
#include <vector>

#include <boost/container/small_vector.hpp>

namespace nix {

/**
 * String tokenizer.
 *
 * See also `basicSplitString()`, which preserves empty strings between separators, as well as at the start and end.
 */
template<class C, class CharT = char>
C basicTokenizeString(std::basic_string_view<CharT> s, std::basic_string_view<CharT> separators);

/**
 * Like `basicTokenizeString` but specialized to the default `char`
 */
template<class C>
C tokenizeString(std::string_view s, std::string_view separators = " \t\n\r");

extern template std::list<std::string> tokenizeString(std::string_view s, std::string_view separators);

/**
 * Split a string, preserving empty strings between separators, as well as at the start and end.
 *
 * Returns a non-empty collection of strings.
 */
template<class C, class CharT = char>
C basicSplitString(std::basic_string_view<CharT> s, std::basic_string_view<CharT> separators);
template<typename C>
C splitString(std::string_view s, std::string_view separators);


/**
 * Concatenate the given strings with a separator between the elements.
 */
template<class C>
std::string concatStringsSep(const std::string_view sep, const C & ss);

/* concatMapStringsSep below collects into this, so it is the one
   container form worth instantiating once rather than per caller. */
extern template std::string
concatStringsSep(std::string_view, const boost::container::small_vector<std::string, 64> &);


/**
 * Apply a function to the `iterable`'s items and concat them with `separator`.
 */
template<class C, class F>
std::string concatMapStringsSep(std::string_view separator, const C & iterable, F fn)
{
    boost::container::small_vector<std::string, 64> strings;
    strings.reserve(iterable.size());
    for (const auto & elem : iterable) {
        strings.push_back(fn(elem));
    }
    return concatStringsSep(separator, strings);
}

/**
 * Ignore any empty strings at the start of the list, and then concatenate the
 * given strings with a separator between the elements.
 *
 * @deprecated This function exists for historical reasons. You probably just
 *             want to use `concatStringsSep`.
 */
/**
 * Check that the string does not contain any NUL bytes and return c_str().
 * @throws Error if str contains '\0' bytes.
 */
const char * requireCString(const std::string & str);

} // namespace nix
