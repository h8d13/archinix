#include "nix/util/util.hh"
#include "nix/util/ref.hh"
#include "nix/util/fmt.hh"
#include "nix/util/signals.hh"

#include <array>
#include <cctype>

#include <boost/lexical_cast.hpp>
#include <stdint.h>

#ifdef NDEBUG
#  error "Nix may not be built with assertions disabled (i.e. with -DNDEBUG)."
#endif

namespace nix {

void FormatError::anchor() {}

void initLibUtil()
{
    // Check that exception handling works. Exception handling has been observed
    // not to work on darwin when the linker flags aren't quite right.
    // In this case we don't want to expose the user to some unrelated uncaught
    // exception, but rather tell them exactly that exception handling is
    // broken.
    // When exception handling fails, the message tends to be printed by the
    // C++ runtime, followed by an abort.
    // For example on macOS we might see an error such as
    // libc++abi: terminating with uncaught exception of type nix::SystemError: error: C++ exception handling is broken.
    // This would appear to be a problem with the way Nix was compiled and/or linked and/or loaded.
    bool caught = false;
    try {
        throwExceptionSelfCheck();
    } catch (const nix::Error & _e) {
        caught = true;
    }
    // This is not actually the main point of this check, but let's make sure anyway:
    assert(caught);

}

//////////////////////////////////////////////////////////////////////

std::string chomp(std::string_view s)
{
    size_t i = s.find_last_not_of(" \n\r\t");
    return i == s.npos ? "" : std::string(s, 0, i + 1);
}

std::string replaceStrings(std::string res, std::string_view from, std::string_view to)
{
    if (from.empty())
        return res;
    size_t pos = 0;
    while ((pos = res.find(from, pos)) != res.npos) {
        res.replace(pos, from.size(), to);
        pos += to.size();
    }
    return res;
}

template<class N>
std::optional<N> string2Int(const std::string_view s)
{
    if (s.substr(0, 1) == "-" && !std::numeric_limits<N>::is_signed)
        return std::nullopt;
    try {
        return boost::lexical_cast<N>(s.data(), s.size());
    } catch (const boost::bad_lexical_cast &) {
        return std::nullopt;
    }
}

// Explicitly instantiated in one place for faster compilation
template std::optional<signed int> string2Int<signed int>(const std::string_view s);

static const int64_t conversionNumber = 1024;

SizeUnit getSizeUnit(int64_t value)
{
    auto unit = sizeUnits.begin();
    uint64_t absValue = std::abs(value);
    while (absValue > conversionNumber && unit < sizeUnits.end()) {
        unit++;
        absValue /= conversionNumber;
    }
    return *unit;
}

std::string renderSizeWithoutUnit(int64_t value, SizeUnit unit, bool align)
{
    // bytes should also displayed as KiB => 100 Bytes => 0.1 KiB
    auto power = std::max<std::underlying_type_t<SizeUnit>>(1, std::to_underlying(unit));
    double denominator = std::pow(conversionNumber, power);
    double result = (double) value / denominator;
    return fmt(align ? "%6.1f" : "%.1f", result);
}

char getSizeUnitSuffix(SizeUnit unit)
{
    switch (unit) {
#define NIX_UTIL_DEFINE_SIZE_UNIT(name, suffix) \
    case SizeUnit::name:                        \
        return suffix;
        NIX_UTIL_SIZE_UNITS
#undef NIX_UTIL_DEFINE_SIZE_UNIT
    }

    assert(false);
}

std::string renderSize(int64_t value, bool align)
{
    SizeUnit unit = getSizeUnit(value);
    return fmt("%s %ciB", renderSizeWithoutUnit(value, unit, align), getSizeUnitSuffix(unit));
}

bool hasPrefix(std::string_view s, std::string_view prefix)
{
    return s.compare(0, prefix.size(), prefix) == 0;
}

void ignoreExceptionInDestructor(Verbosity lvl)
{
    /* Make sure no exceptions leave this function.
       printError() also throws when remote is closed. */
    try {
        try {
            throw;
        } catch (Error & e) {
            printMsg(lvl, ANSI_RED "error (ignored):" ANSI_NORMAL " %s", e.info().msg);
        } catch (std::exception & e) {
            printMsg(lvl, ANSI_RED "error (ignored):" ANSI_NORMAL " %s", e.what());
        }
    } catch (...) {
    }
}

void ignoreExceptionExceptInterrupt(Verbosity lvl)
{
    try {
        throw;
    } catch (const Interrupted & e) {
        throw;
    } catch (const Cancelled & e) {
        /* Morally the same as Interrupted, just not triggered by a user but some other
           cancellation. */
        throw;
    } catch (Error & e) {
        printMsg(lvl, ANSI_RED "error (ignored):" ANSI_NORMAL " %s", e.info().msg);
    } catch (std::exception & e) {
        printMsg(lvl, ANSI_RED "error (ignored):" ANSI_NORMAL " %s", e.what());
    }
}

} // namespace nix
