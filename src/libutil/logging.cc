#include "nix/util/logging.hh"
#include "nix/util/file-descriptor.hh"

#include <sstream>

namespace nix {

Verbosity verbosity = lvlInfo;

/**
 * This is a raw pointer to allow it to leak: it outlives every
 * destructor that might still want to log.
 */
Logger * logger = new Logger();

static void writeFullLogging(Descriptor fd, std::string_view s)
{
    try {
        writeFull(fd, s, false);
    } catch (SystemError & e) {
        /* Ignore failing logging writes.  We need to ignore write
           errors to ensure that cleanup code that writes logs runs
           to completion if the other side of the logging fd has
           been closed unexpectedly. */
    }
}

static void writeToStderr(std::string_view s)
{
    writeFullLogging(getStandardError(), s);
}

void Logger::log(Verbosity lvl, std::string_view s)
{
    if (lvl > verbosity)
        return;

    writeToStderr(std::string(s) + "\n");
}

void Logger::logEI(const ErrorInfo & ei)
{
    std::ostringstream oss;
    showErrorInfo(oss, ei);

    log(ei.level, oss.view());
}

void Logger::warn(const std::string & msg)
{
    log(lvlWarn, "warning: " + msg);
}

} // namespace nix
