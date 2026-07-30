#pragma once
///@file

#include <boost/format.hpp>
#include <string>
#include <filesystem>

namespace nix {

/**
 * A helper for writing `boost::format` expressions.
 *
 * These are equivalent:
 *
 * ```
 * formatHelper(formatter, a_0, ..., a_n)
 * formatter % a_0 % ... % a_n
 * ```
 *
 * With a single argument, `formatHelper(s)` is a no-op.
 */
template<class F>
inline void formatHelper(F & f)
{
}

template<class F, typename T, typename... Args>
inline void formatHelper(F & f, const T & x, const Args &... args)
{
    static_assert(!std::is_same_v<std::remove_cvref_t<T>, std::filesystem::path>);
    static_assert(!(std::is_same_v<std::remove_cvref_t<Args>, std::filesystem::path> || ...));
    // Interpolate one argument and then recurse.
    formatHelper(f % x, args...);
}

/**
 * Set the correct exceptions for `fmt`.
 */
inline void setExceptions(boost::format & fmt)
{
    fmt.exceptions(boost::io::all_error_bits ^ boost::io::too_many_args_bit ^ boost::io::too_few_args_bit);
}

/**
 * A helper for writing a `boost::format` expression to a string.
 *
 * These are (roughly) equivalent:
 *
 * ```
 * fmt(formatString, a_0, ..., a_n)
 * (boost::format(formatString) % a_0 % ... % a_n).str()
 * ```
 *
 * However, when called with a single argument, the string is returned
 * unchanged.
 *
 * If you write code like this:
 *
 * ```
 * std::cout << boost::format(stringFromUserInput) << std::endl;
 * ```
 *
 * And `stringFromUserInput` contains formatting placeholders like `%s`, then
 * the code will crash at runtime. `fmt` helps you avoid this pitfall.
 */
inline std::string fmt(const std::string & s)
{
    return s;
}

inline std::string fmt(std::string_view s)
{
    return std::string(s);
}

inline std::string fmt(const char * s)
{
    return s;
}

template<typename... Args>
inline std::string fmt(const std::string & fs, const Args &... args)
{
    boost::format f(fs);
    setExceptions(f);
    formatHelper(f, args...);
    return f.str();
}

/**
 * All std::filesystem::path values must be wrapped in this class when formatting via HintFmt
 * or fmt(). This avoids accidentail double-quoting due to the standard operator<< implementation
 * for std::filesystem::path.
 */
struct PathFmt
{
    explicit PathFmt(const std::filesystem::path & p)
    {
        value = p.string();
    }

    std::string value;
};

inline std::ostream & operator<<(std::ostream & out, const PathFmt & y)
{
    return out << "\"" << y.value << "\"";
}

/**
 * A wrapper around `boost::format` carrying an error message.
 *
 * Upstream wrapped every interpolated argument in `Magenta`, with an
 * `Uncolored` wrapper to opt back out. Nothing here wants per-argument
 * colour, so both are gone and `operator%` interpolates plainly.
 */
class HintFmt
{
private:
    boost::format fmt;

public:
    /**
     * Format the given string literally, without interpolating format
     * placeholders.
     */
    HintFmt(const std::string & literal)
        : HintFmt("%s", literal)
    {
    }

    /**
     * Interpolate the given arguments into the format string.
     */
    template<typename... Args>
    HintFmt(const std::string & format, const Args &... args)
        : HintFmt(boost::format(format), args...)
    {
    }

    HintFmt(const HintFmt & hf)
        : fmt(hf.fmt)
    {
    }

    /* boost::format has no move ctor, so this copies either way */
    template<typename... Args>
    HintFmt(boost::format && fmt, const Args &... args)
        : fmt(fmt)
    {
        static_assert(!(std::is_same_v<std::remove_cvref_t<Args>, std::filesystem::path> || ...));
        setExceptions(fmt);
        formatHelper(*this, args...);
    }

    HintFmt & operator%(const std::filesystem::path & value) = delete;

    template<class T>
    HintFmt & operator%(const T & value)
    {
        fmt % value;
        return *this;
    }

    HintFmt & operator=(HintFmt const & rhs) = default;

    std::string str() const
    {
        return fmt.str();
    }
};

std::ostream & operator<<(std::ostream & os, const HintFmt & hf);

} // namespace nix
