#include "nix/util/terminal.hh"
#include "nix/util/environment-variables.hh"

#include <unistd.h>

namespace nix {

bool isTTY()
{
    static const bool tty = isatty(STDERR_FILENO) && getEnv("TERM").value_or("dumb") != "dumb"
                            && !(getEnv("NO_COLOR").has_value() || getEnv("NOCOLOR").has_value());

    return tty;
}

std::string filterANSIEscapes(std::string_view s, bool filterAll)
{
    std::string t;
    size_t w = 0; /* printable characters, for tab stops */
    auto i = s.begin();

    while (i != s.end()) {

        if (*i == '\e') {
            std::string e;
            e += *i++;
            char last = 0;

            if (i != s.end() && *i == '[') {
                e += *i++;
                // eat parameter bytes
                while (i != s.end() && *i >= 0x30 && *i <= 0x3f)
                    e += *i++;
                // eat intermediate bytes
                while (i != s.end() && *i >= 0x20 && *i <= 0x2f)
                    e += *i++;
                // eat final byte
                if (i != s.end() && *i >= 0x40 && *i <= 0x7e)
                    e += last = *i++;
            } else if (i != s.end() && *i == ']') {
                // OSC
                e += *i++;
                // https://gist.github.com/egmontkob/eb114294efbcd5adb1944c9f3cb5feda defines
                // two forms of a URI separator:
                // 1. ESC '\\' (standard)
                // 2. BEL ('\a') (xterm-style, used by gcc)

                // eat ESC or BEL
                while (i != s.end() && *i != '\e' && *i != '\a')
                    e += *i++;
                if (i != s.end()) {
                    char v = *i;
                    e += *i++;
                    // eat backslash after ESC
                    if (i != s.end() && v == '\e' && *i == '\\')
                        e += last = *i++;
                }
            } else {
                if (i != s.end() && *i >= 0x40 && *i <= 0x5f)
                    e += *i++;
            }

            if (!filterAll && last == 'm')
                t += e;
        }

        else if (*i == '\t') {
            // expand to the next 8-column tab stop
            do {
                ++w;
                t += ' ';
            } while (w % 8);
            i++;
        }

        else if (*i == '\r' || *i == '\a')
            // do nothing for now
            i++;

        else {
            /* count characters rather than bytes so tab stops still
               line up in UTF-8 text; display width (CJK/emoji double
               width) is not tracked, nothing truncates any more */
            if ((*i & 0xc0) != 0x80)
                ++w;
            t += *i++;
        }
    }
    return t;
}

} // namespace nix
