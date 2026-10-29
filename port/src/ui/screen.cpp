#include "screen.h"
#include "../platform/platform.h"

#include <cstdarg>
#include <cstdio>

namespace idas3 {
namespace ui {

Screen::Screen() : cols_(platform::consoleCols()), rows_(platform::consoleRows()) {}

static std::string vformat(const char* fmt, va_list ap)
{
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    return buf;
}

void Screen::line(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    body_.push_back(vformat(fmt, ap));
    va_end(ap);
}

void Screen::footer(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    footer_.push_back(vformat(fmt, ap));
    va_end(ap);
}

int Screen::freeRows() const
{
    return rows_ - (int)footer_.size() - (int)body_.size();
}

size_t visibleLength(const std::string& s)
{
    size_t n = 0;
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] == '\x1b')
        {
            while (i < s.size() && !((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z')))
                i++;
            continue;
        }
        n++;
    }
    return n;
}

std::string clipVisible(const std::string& s, int cols)
{
    std::string out;
    int n = 0;
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] == '\x1b')
        {
            size_t j = i;
            while (j < s.size() && !((s[j] >= 'A' && s[j] <= 'Z') || (s[j] >= 'a' && s[j] <= 'z')))
                j++;
            out.append(s, i, j - i + 1);
            i = j;
            continue;
        }
        if (n >= cols)
            continue;   // keep scanning: later escapes (colour resets) still apply
        out += s[i];
        n++;
    }
    return out;
}

std::string Screen::compose() const
{
    std::vector<std::string> rows;
    const int bodyRows = rows_ - (int)footer_.size();
    for (int i = 0; i < (int)body_.size() && i < bodyRows; i++)
        rows.push_back(body_[i]);
    while ((int)rows.size() < bodyRows)
        rows.push_back(std::string());
    for (const std::string& f : footer_)
        rows.push_back(f);

    std::string out;
    for (size_t i = 0; i < rows.size(); i++)
    {
        out += clipVisible(rows[i], cols_) + UI_RESET;
        if (i + 1 < rows.size())
            out += '\n';
    }
    return out;
}

void Screen::present() const
{
    platform::present(compose());
}

} // namespace ui
} // namespace idas3
