#include "nix/util/logging.hh"
#include "nix/util/file-descriptor.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/terminal.hh"

#include <sstream>

namespace nix {

LoggerSettings loggerSettings;

Verbosity verbosity = lvlInfo;

/**
 * This is a raw pointer to allow it to leak: it outlives every
 * destructor that might still want to log.
 */
Logger * logger = new Logger();

Logger::Logger()
{
    systemd = getEnv("IN_SYSTEMD") == "1";
    tty = isTTY();
}

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

void writeToStderr(std::string_view s)
{
    writeFullLogging(getStandardError(), s);
}

void Logger::log(Verbosity lvl, std::string_view s)
{
    if (lvl > verbosity)
        return;

    std::string prefix;

    if (systemd) {
        char c;
        switch (lvl) {
        case lvlError:
            c = '3';
            break;
        case lvlWarn:
            c = '4';
            break;
        case lvlNotice:
        case lvlInfo:
            c = '5';
            break;
        case lvlTalkative:
        case lvlChatty:
            c = '6';
            break;
        case lvlDebug:
        case lvlVomit:
            c = '7';
            break;
        default:
            c = '7';
            break; // should not happen, and missing enum case is reported by -Werror=switch-enum
        }
        prefix = std::string("<") + c + ">";
    }

    writeToStderr(prefix + filterANSIEscapes(s, !tty) + "\n");
}

void Logger::logEI(const ErrorInfo & ei)
{
    std::ostringstream oss;
    showErrorInfo(oss, ei, loggerSettings.showTrace);

    log(ei.level, oss.view());
}

void Logger::warn(const std::string & msg)
{
    log(lvlWarn, ANSI_WARNING "warning:" ANSI_NORMAL " " + msg);
}

} // namespace nix
