#include "nix/util/logging.hh"
#include "nix/util/file-descriptor.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/terminal.hh"
#include "nix/util/util.hh"
#include "nix/util/sync.hh"

#include <atomic>
#include <sstream>

namespace nix {

LoggerSettings loggerSettings;


static thread_local ActivityId curActivity = 0;

ActivityId getCurActivity()
{
    return curActivity;
}

void setCurActivity(const ActivityId activityId)
{
    curActivity = activityId;
}

/**
 * This is a raw pointer to allow it to leak.
 * Avoids races in activity teardown.
 */
Logger * logger = makeSimpleLogger(true).release();

Logger::~Logger() {}

void Logger::warn(const std::string & msg)
{
    log(lvlWarn, ANSI_WARNING "warning:" ANSI_NORMAL " " + msg);
}

void Logger::writeToStdout(std::string_view s)
{
    Descriptor standard_out = getStandardOutput();
    writeFull(standard_out, s);
    writeFull(standard_out, "\n");
}

namespace {

class SimpleLogger : public Logger
{
public:

    bool systemd, tty;
    bool printBuildLogs;

    SimpleLogger(bool printBuildLogs)
        : printBuildLogs(printBuildLogs)
    {
        systemd = getEnv("IN_SYSTEMD") == "1";
        tty = isTTY();
    }

    bool isVerbose() override
    {
        return printBuildLogs;
    }

    void log(Verbosity lvl, std::string_view s) override
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

    void logEI(const ErrorInfo & ei) override
    {
        std::ostringstream oss;
        showErrorInfo(oss, ei, loggerSettings.showTrace);

        log(ei.level, oss.view());
    }

    void startActivity(
        ActivityId act,
        Verbosity lvl,
        ActivityType type,
        const std::string & s,
        const Fields & fields,
        ActivityId parent) override
    {
        if (lvl <= verbosity && !s.empty())
            log(lvl, s + "...");
    }

    void result(ActivityId act, ResultType type, const Fields & fields) override
    {
        if (type == resBuildLogLine && printBuildLogs) {
            auto lastLine = fields[0].s;
            printError(lastLine);
        } else if (type == resPostBuildLogLine && printBuildLogs) {
            auto lastLine = fields[0].s;
            printError("post-build-hook: " + lastLine);
        }
    }
};

} // namespace

Verbosity verbosity = lvlInfo;

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

std::unique_ptr<Logger> makeSimpleLogger(bool printBuildLogs)
{
    return std::make_unique<SimpleLogger>(printBuildLogs);
}

std::atomic<uint64_t> nextId{0};

static uint64_t getPid()
{
    return getpid();
}

Activity::Activity(
    Logger & logger,
    Verbosity lvl,
    ActivityType type,
    const std::string & s,
    const Logger::Fields & fields,
    ActivityId parent)
    : logger(logger)
    , id(nextId++ + (((uint64_t) getPid()) << 32))
{
    logger.startActivity(id, lvl, type, s, fields, parent);
}

Activity::~Activity()
{
    try {
        logger.stopActivity(id);
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

} // namespace nix
