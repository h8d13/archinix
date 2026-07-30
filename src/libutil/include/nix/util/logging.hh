#pragma once
///@file

#include "nix/util/error.hh"
#include "nix/util/file-descriptor.hh"
#include "nix/util/finally.hh"
#include "nix/util/fun.hh"

#include <filesystem>


namespace nix {

/**
 * stderr logger. One implementation, so no vtable: `logger` is the
 * process-wide instance the printMsg/logError macros write through.
 *
 * `systemd` prefixes lines with a syslog level (<3>..<7>) when running
 * under a unit, which is how nixgen-*.service lines get their priority
 * in the journal; `tty` decides whether ANSI escapes survive.
 */
class Logger
{
public:

    bool systemd, tty;

    Logger();

    void log(Verbosity lvl, std::string_view s);

    void log(std::string_view s)
    {
        log(lvlInfo, s);
    }

    void logEI(const ErrorInfo & ei);

    void logEI(Verbosity lvl, ErrorInfo ei)
    {
        ei.level = lvl;
        logEI(ei);
    }

    void warn(const std::string & msg);

};

extern Logger * logger;

/**
 * suppress msgs > this
 */
extern Verbosity verbosity;

/**
 * Print a message with the standard ErrorInfo format.
 * In general, use these 'log' macros for reporting problems that may require user
 * intervention or that need more explanation.  Use the 'print' macros for more
 * lightweight status messages.
 */
#define logErrorInfo(level, errorInfo...)      \
    do {                                       \
        if ((level) <= nix::verbosity) {       \
            logger->logEI((level), errorInfo); \
        }                                      \
    } while (0)

#define logError(errorInfo...) logErrorInfo(lvlError, errorInfo)
#define logWarning(errorInfo...) logErrorInfo(lvlWarn, errorInfo)

/**
 * Print a string message if the current log level is at least the specified
 * level. Note that this has to be implemented as a macro to ensure that the
 * arguments are evaluated lazily.
 */
#define printMsgUsing(loggerParam, level, args...) \
    do {                                           \
        auto __lvl = level;                        \
        if (__lvl <= nix::verbosity) {             \
            loggerParam->log(__lvl, fmt(args));    \
        }                                          \
    } while (0)
#define printMsg(level, args...) printMsgUsing(logger, level, args)

#define printError(args...) printMsg(lvlError, args)
#define notice(args...) printMsg(lvlNotice, args)
#define printInfo(args...) printMsg(lvlInfo, args)
#define printTalkative(args...) printMsg(lvlTalkative, args)
#define debug(args...) printMsg(lvlDebug, args)
#define vomit(args...) printMsg(lvlVomit, args)

/**
 * if verbosity >= lvlWarn, print a message with a yellow 'warning:' prefix.
 */
template<typename... Args>
inline void warn(const std::string & fs, const Args &... args)
{
    boost::format f(fs);
    formatHelper(f, args...);
    logger->warn(f.str());
}

#define warnOnce(haveWarned, args...) \
    if (!haveWarned) {                \
        haveWarned = true;            \
        warn(args);                   \
    }

} // namespace nix
