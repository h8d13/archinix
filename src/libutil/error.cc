#include <algorithm>

#include "nix/util/error.hh"
#include "nix/util/signals.hh"
#include "nix/util/util.hh"
#include "nix/util/file-system.hh"

#include <cinttypes>
#include <iostream>
#include <optional>
#include <sstream>

namespace nix {

void BaseError::anchor() {}

void Error::anchor() {}

void UsageError::anchor() {}

void UnimplementedError::anchor() {}

void SystemError::anchor() {}

void SysError::anchor() {}

void BaseError::addTrace(HintFmt hint, TracePrint print)
{
    err.traces.push_front(Trace{.hint = hint, .print = print});
}

void throwExceptionSelfCheck()
{
    // This is meant to be caught in initLibUtil()
    throw Error(
        "C++ exception handling is broken. This would appear to be a problem with the way Nix was compiled and/or linked and/or loaded.");
}

// c++ std::exception descendants must have a 'const char* what()' function.
// This stringifies the error and caches it for use by what(), or similarly by msg().
const std::string & BaseError::calcWhat() const
{
    if (!what_.has_value()) {
        std::ostringstream oss;
        showErrorInfo(oss, err);
        what_ = oss.str();
    }
    return *what_;
}

std::ostream & operator<<(std::ostream & os, const HintFmt & hf)
{
    return os << hf.str();
}

/**
 * An arbitrarily defined value comparison for the purpose of using traces in the key of a sorted container.
 */
inline std::strong_ordering operator<=>(const Trace & lhs, const Trace & rhs)
{
    // This formats a freshly formatted hint string and then throws it away, which
    // shouldn't be much of a problem because it only runs infrequently, for
    // trace printing.
    return lhs.hint.str() <=> rhs.hint.str();
}

static std::string indent(std::string_view indentFirst, std::string_view indentRest, std::string_view s)
{
    std::string res;
    bool first = true;

    while (!s.empty()) {
        auto end = s.find('\n');
        if (!first)
            res += "\n";
        res += chomp(std::string(first ? indentFirst : indentRest) + std::string(s.substr(0, end)));
        first = false;
        if (end == s.npos)
            break;
        s = s.substr(end + 1);
    }

    return res;
}

static void printTrace(std::ostream & output, const std::string_view & indent, size_t & count, const Trace & trace)
{
    output << "\n" << "… " << trace.hint.str() << "\n";
}

void printSkippedTracesMaybe(
    std::ostream & output,
    const std::string_view & indent,
    size_t & count,
    std::vector<Trace> & skippedTraces,
    std::set<Trace> tracesSeen)
{
    if (skippedTraces.size() > 0) {
        // If we only skipped a few frames, print them out normally;
        // messages like "1 duplicate frames omitted" aren't helpful.
        if (skippedTraces.size() <= 5) {
            for (auto & trace : skippedTraces) {
                printTrace(output, indent, count, trace);
            }
        } else {
            output << "\n"
                   << "(" << skippedTraces.size() << " duplicate frames omitted)" << "\n";
            // Clear the set of "seen" traces after printing a chunk of
            // `duplicate frames omitted`.
            //
            // Consider a mutually recursive stack trace with:
            // - 10 entries of A
            // - 10 entries of B
            // - 10 entries of A
            //
            // If we don't clear `tracesSeen` here, we would print output like this:
            // - 1 entry of A
            // - (9 duplicate frames omitted)
            // - 1 entry of B
            // - (19 duplicate frames omitted)
            //
            // This would obscure the control flow, which went from A,
            // to B, and back to A again.
            //
            // In contrast, if we do clear `tracesSeen`, the output looks like this:
            // - 1 entry of A
            // - (9 duplicate frames omitted)
            // - 1 entry of B
            // - (9 duplicate frames omitted)
            // - 1 entry of A
            // - (9 duplicate frames omitted)
            //
            // See: `tests/functional/lang/eval-fail-mutual-recursion.nix`
            tracesSeen.clear();
        }
    }
    // We've either printed each trace in `skippedTraces` normally, or
    // printed a chunk of `duplicate frames omitted`. Either way, we've
    // processed these traces and can clear them.
    skippedTraces.clear();
}

std::ostream & showErrorInfo(std::ostream & out, const ErrorInfo & einfo)
{
    std::string prefix;
    switch (einfo.level) {
    case Verbosity::lvlError: {
        prefix = "error";
        break;
    }
    case Verbosity::lvlNotice: {
        prefix = "note";
        break;
    }
    case Verbosity::lvlWarn: {
        prefix = "warning";
        break;
    }
    case Verbosity::lvlInfo: {
        prefix = "info";
        break;
    }
    case Verbosity::lvlTalkative: {
        prefix = "talk";
        break;
    }
    case Verbosity::lvlVomit: {
        prefix = "vomit";
        break;
    }
    case Verbosity::lvlDebug: {
        prefix = "debug";
        break;
    }
    default:
        assert(false);
    }

    prefix += ": ";

    std::ostringstream oss;

    /* Traces are capped at 3. Upstream made the cap conditional on
       --show-trace, a flag that both lifted the cap and made the
       evaluator collect more traces to begin with; neither the flag nor
       an evaluator exists here, so the cap is unconditional. A trace
       with a position counts as two towards it. `TracePrint::Always`
       still overrides. */

    // Enough indent to align with with the `... `
    // prepended to each element of the trace
    auto ellipsisIndent = "  ";

    if (!einfo.traces.empty()) {
        // Stack traces seen since we last printed a chunk of `duplicate frames
        // omitted`.
        std::set<Trace> tracesSeen;
        // A consecutive sequence of stack traces that are all in `tracesSeen`.
        std::vector<Trace> skippedTraces;
        size_t count = 0;
        bool truncate = false;

        for (const auto & trace : einfo.traces) {
            if (trace.hint.str().empty())
                continue;

            if (count > 3) {
                truncate = true;
            }

            if (!truncate || trace.print == TracePrint::Always) {

                if (tracesSeen.count(trace)) {
                    skippedTraces.push_back(trace);
                    continue;
                }

                tracesSeen.insert(trace);

                printSkippedTracesMaybe(oss, ellipsisIndent, count, skippedTraces, tracesSeen);

                count++;

                printTrace(oss, ellipsisIndent, count, trace);
            }
        }

        printSkippedTracesMaybe(oss, ellipsisIndent, count, skippedTraces, tracesSeen);

        if (truncate) {
            oss << "\n" << "(stack trace truncated)" << "\n";
        }

        oss << "\n" << prefix;
    }

    oss << einfo.msg << "\n";

    out << indent(prefix, std::string(prefix.size(), ' '), chomp(oss.str()));

    return out;
}

/** Write to stderr in a robust and minimal way, considering that the process
 * may be in a bad state.
 */
static void writeErr(std::string_view buf)
{
    Descriptor fd = getStandardError();
    while (!buf.empty()) {
        auto n = ::write(fd, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR)
                continue;
            abort();
        }
        buf = buf.substr(n);
    }
}

void panic(std::string_view msg)
{
    writeErr("\n\nterminating due to unexpected unrecoverable internal error: ");
    writeErr(msg);
    writeErr("\n");
    std::terminate();
}

void unreachable(std::source_location loc)
{
    char buf[512];
    int n = snprintf(
        buf,
        sizeof(buf),
        "Unexpected condition in %s at %s:%" PRIuLEAST32,
        loc.function_name(),
        loc.file_name(),
        loc.line());
    if (n < 0)
        panic("Unexpected condition and could not format error message");
    panic(std::string_view(buf, std::min(static_cast<int>(sizeof(buf)), n)));
}

} // namespace nix
