#pragma once
///@file

#include "nix/util/types.hh"
#include "nix/util/error.hh"
#include "nix/util/logging.hh"
#include "nix/util/strings.hh"

#include <functional>
#include <map>
#include <sstream>
#include <bit>
#include <optional>

namespace nix {

void initLibUtil();

MakeError(FormatError, Error);

template<class... Parts>
auto concatStrings(Parts &&... parts)
    -> std::enable_if_t<(... && std::is_convertible_v<Parts, std::string_view>), std::string>
{
    std::string_view views[sizeof...(parts)] = {std::forward<Parts>(parts)...};
    return concatStringsSep({}, views);
}

/**
 * Remove trailing whitespace from a string.
 *
 * \todo return std::string_view.
 */
std::string chomp(std::string_view s);

/**
 * Replace all occurrences of a string inside another string.
 */
std::string replaceStrings(std::string s, std::string_view from, std::string_view to);

/**
 * Parse a string into an integer.
 */
template<class N>
std::optional<N> string2Int(const std::string_view s);

// Base also uses 'K', because it should also displayed as KiB => 100 Bytes => 0.1 KiB
#define NIX_UTIL_SIZE_UNITS               \
    NIX_UTIL_DEFINE_SIZE_UNIT(Base, 'K')  \
    NIX_UTIL_DEFINE_SIZE_UNIT(Kilo, 'K')  \
    NIX_UTIL_DEFINE_SIZE_UNIT(Mega, 'M')  \
    NIX_UTIL_DEFINE_SIZE_UNIT(Giga, 'G')  \
    NIX_UTIL_DEFINE_SIZE_UNIT(Tera, 'T')  \
    NIX_UTIL_DEFINE_SIZE_UNIT(Peta, 'P')  \
    NIX_UTIL_DEFINE_SIZE_UNIT(Exa, 'E')   \
    NIX_UTIL_DEFINE_SIZE_UNIT(Zetta, 'Z') \
    NIX_UTIL_DEFINE_SIZE_UNIT(Yotta, 'Y')

enum class SizeUnit {
#define NIX_UTIL_DEFINE_SIZE_UNIT(name, suffix) name,
    NIX_UTIL_SIZE_UNITS
#undef NIX_UTIL_DEFINE_SIZE_UNIT
};

constexpr inline auto sizeUnits = std::to_array<SizeUnit>({
#define NIX_UTIL_DEFINE_SIZE_UNIT(name, suffix) SizeUnit::name,
    NIX_UTIL_SIZE_UNITS
#undef NIX_UTIL_DEFINE_SIZE_UNIT
});

SizeUnit getSizeUnit(int64_t value);

std::string renderSizeWithoutUnit(int64_t value, SizeUnit unit, bool align = false);

char getSizeUnitSuffix(SizeUnit unit);

/**
 * Pretty-print a byte value, e.g. 12433615056 is rendered as `11.6
 * GiB`. If `align` is set, the number will be right-justified by
 * padding with spaces on the left.
 */
std::string renderSize(int64_t value, bool align = false);

/**
 * Convert a little-endian integer to host order.
 */
template<typename T>
T readLittleEndian(unsigned char * p)
{
    T x = 0;
    /* Byte types such as char/unsigned char/std::byte are a bit special because
       they are allowed to alias anything else. Thus a raw loop iterating
       over the bytes here would be quite inefficient and iterate byte-by-byte
       (the compiler cannot optimise anything because the pointer might alias
       something). Use a memcpy + byteswap here as needed. */
    std::memcpy(&x, p, sizeof(T));
    /* Don't need to do anything if we are not on a big endian machine. */
    if constexpr (std::endian::native != std::endian::little) {
        if constexpr (std::is_same_v<T, uint64_t>) {
            x = __builtin_bswap64(x);
        } else if constexpr (std::is_same_v<T, uint32_t>) {
            x = __builtin_bswap32(x);
        } else if constexpr (std::is_same_v<T, uint16_t>) {
            x = __builtin_bswap16(x);
        } else {
            /* Signed types don't make their way here. Though it would be fine
               since C++20 mandates 2's complement representation. */
            static_assert(false);
        }
    }
    return x;
}

/**
 * @return true iff `s` starts with `prefix`.
 */
bool hasPrefix(std::string_view s, std::string_view prefix);


/**
 * Exception handling in destructors: print an error message, then
 * ignore the exception.
 *
 * If you're not in a destructor, you usually want to use `ignoreExceptionExceptInterrupt()`.
 *
 * This function might also be used in callbacks whose caller may not handle exceptions,
 * but ideally we propagate the exception using an exception_ptr in such cases.
 * See e.g. `PackBuilderContext`
 */
void ignoreExceptionInDestructor(Verbosity lvl = lvlError);

/**
 * Not destructor-safe.
 * Print an error message, then ignore the exception.
 * If the exception is an `Interrupted` exception, rethrow it.
 *
 * This may be used in a few places where Interrupt can't happen, but that's ok.
 */
void ignoreExceptionExceptInterrupt(Verbosity lvl = lvlError);

/**
 * Get a pointer to the contents of a `std::optional` if it is set, or a
 * null pointer otherise.
 *
 * Const version.
 */
template<class T>
const T * get(const std::optional<T> & opt)
{
    return opt ? &*opt : nullptr;
}

/**
 * Non-const counterpart of `const T * get(const std::optional<T>)`.
 * Takes a mutable reference, but returns a mutable pointer.
 */
template<class T>
T * get(std::optional<T> & opt)
{
    return opt ? &*opt : nullptr;
}

/**
 * Get a value for the specified key from an associate container.
 */
template<class T, typename K>
const typename T::mapped_type * get(const T & map, const K & key)
{
    auto i = map.find(key);
    if (i == map.end())
        return nullptr;
    return &i->second;
}

template<class T, typename K>
typename T::mapped_type * get(T & map, const K & key)
{
    auto i = map.find(key);
    if (i == map.end())
        return nullptr;
    return &i->second;
}

/**
 * Deleted because this is use-after-free liability. Just don't pass temporaries to this overload set.
 */
template<class T, typename K>
typename T::mapped_type * get(T && map, const K & key) = delete;

/**
 * Remove and return the first item from a container.
 */
template<class T>
std::optional<typename T::value_type> pop(T & c)
{
    if (c.empty())
        return {};
    auto v = std::move(c.front());
    c.pop();
    return v;
}

/**
 * Append items to a container. TODO: remove this once we can use
 * C++23's `append_range()`.
 */
template<class C, typename T>
void append(C & c, std::initializer_list<T> l)
{
    c.insert(c.end(), l.begin(), l.end());
}

/**
 * C++17 std::visit boilerplate
 */
template<class... Ts>
struct overloaded : Ts...
{
    using Ts::operator()...;
};
template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

/**
 * Provide an addition operator between strings and string_views
 * inexplicably omitted from the standard library.
 */
inline std::string operator+(const std::string & s1, std::string_view s2)
{
    std::string s;
    s.reserve(s1.size() + s2.size());
    s.append(s1);
    s.append(s2);
    return s;
}

inline std::string operator+(std::string && s, std::string_view s2)
{
    s.append(s2);
    return std::move(s);
}

inline std::string operator+(std::string_view s1, const char * s2)
{
    auto s2Size = strlen(s2);
    std::string s;
    s.reserve(s1.size() + s2Size);
    s.append(s1);
    s.append(s2, s2Size);
    return s;
}

} // namespace nix
