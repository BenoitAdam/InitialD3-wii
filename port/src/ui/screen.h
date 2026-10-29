// One text screen, built line by line and handed to platform::present().
// Lines are clipped to the console width (ANSI colour sequences do not count)
// and footer lines are pinned to the bottom rows, nullDC4Wii style.
#pragma once

#include <string>
#include <vector>

namespace idas3 {
namespace ui {

// ANSI colours, understood by both the libogc console and Windows' VT mode.
#define UI_RESET "\x1b[0m"
#define UI_GREEN "\x1b[32m"
#define UI_RED "\x1b[31m"
#define UI_YELLOW "\x1b[33m"
#define UI_CYAN "\x1b[36m"
#define UI_WHITE "\x1b[37m"

class Screen
{
public:
    Screen();

    void line(const char* fmt, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 2, 3)))
#endif
        ;
    void blank() { body_.push_back(std::string()); }
    void footer(const char* fmt, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 2, 3)))
#endif
        ;

    int cols() const { return cols_; }
    int rows() const { return rows_; }
    // Body rows still free above the footer.
    int freeRows() const;

    std::string compose() const;
    void present() const;

private:
    int cols_;
    int rows_;
    std::vector<std::string> body_;
    std::vector<std::string> footer_;
};

// Visible length of a string, skipping ANSI escape sequences.
size_t visibleLength(const std::string& s);
// Clips to `cols` visible characters (escape sequences are kept).
std::string clipVisible(const std::string& s, int cols);

} // namespace ui
} // namespace idas3
