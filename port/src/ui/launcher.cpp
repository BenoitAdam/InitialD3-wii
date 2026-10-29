#include "launcher.h"
#include "screen.h"
#include "../core/bios_image.h"
#include "../core/game_session.h"
#include "../game/game_runner.h"   // RunOptions (runGame itself is Wii only)
#include "../core/log.h"
#include "../platform/platform.h"
#include "../version.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace idas3 {
namespace ui {

using namespace platform;

namespace {

struct BrowserEntry
{
    std::string name;
    std::string path;
    bool isDir = false;
    uint64_t size = 0;
};

enum class State
{
    Browser,
    Check,
    Options,
    Launch,
    Quit,
};

class Launcher
{
public:
    int run();

private:
    State browser();
    State check();
    State options();
    State launch();
    State autoLaunch();

    void loadOptions();
    void saveOptions() const;
    void openStorageRoot();
    void refreshListing();
    void header(Screen& s, const std::string& subtitle);
    void drawCheckLines(Screen& s, const std::vector<CheckLine>& lines, size_t first, const std::string& current);
    GameSession::Progress progressDrawer(const std::string& title, size_t firstLine);

    // Browser.
    std::string root_;
    std::string path_;
    std::vector<BrowserEntry> entries_;
    int selected_ = 0;
    bool picHere_ = false;
    std::string message_;

    // Session.
    GameSession session_;
    std::string chdPath_;
    bool needCheck_ = true;
    RunOptions run_;                 // CPU + core presets for the launch
    int optionPage_ = PAGE_GENERAL;
    int optionRow_ = 0;
    bool biosSearched_ = false;
    bool haveBios_ = false;
    BiosImage bios_;
    std::string biosError_;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string formatSize(uint64_t bytes)
{
    char buf[32];
    if (bytes >= 1024ull * 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.2f GB", (double)bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1f MB", (double)bytes / (1024.0 * 1024));
    else
        snprintf(buf, sizeof(buf), "%u KB", (unsigned)((bytes + 1023) / 1024));
    return buf;
}

bool hasExtension(const std::string& name, const char* ext)
{
    const std::string lower = toLower(name);
    const size_t n = strlen(ext);
    return lower.size() >= n && lower.compare(lower.size() - n, n, ext) == 0;
}

// Parent of a folder path ending with a separator; empty at a device root.
std::string parentOf(const std::string& dir)
{
    if (dir.size() < 2)
        return std::string();
    std::string d = dir.substr(0, dir.size() - 1);
    if (!d.empty() && d.back() == ':')
        return std::string();
    const size_t s = d.find_last_of("/\\");
    return s == std::string::npos ? std::string() : d.substr(0, s + 1);
}

const char* statusTag(CheckStatus st)
{
    switch (st)
    {
    case CheckStatus::Ok: return UI_GREEN "[ OK ]" UI_RESET;
    case CheckStatus::Warn: return UI_YELLOW "[WARN]" UI_RESET;
    case CheckStatus::Fail: return UI_RED "[FAIL]" UI_RESET;
    case CheckStatus::Info: return UI_CYAN "[INFO]" UI_RESET;
    }
    return "[    ]";
}

// ---------------------------------------------------------------------------
// Common drawing
// ---------------------------------------------------------------------------

void Launcher::header(Screen& s, const std::string& subtitle)
{
    s.blank();
    s.line(UI_WHITE "%s" UI_RESET " - %s   [%s]", IDAS3_PORT_NAME, IDAS3_PORT_VERSION, platform::name());
    s.line("%s", subtitle.c_str());
    s.blank();
}

void Launcher::drawCheckLines(Screen& s, const std::vector<CheckLine>& lines, size_t first,
                              const std::string& current)
{
    // Keep the newest lines when they do not all fit.
    const int reserve = current.empty() ? 0 : 2;
    const int room = std::max(1, s.freeRows() - reserve);
    size_t begin = first;
    if (lines.size() > first && (int)(lines.size() - first) > room)
        begin = lines.size() - (size_t)room;
    for (size_t i = begin; i < lines.size(); i++)
        s.line("%s %s", statusTag(lines[i].status), lines[i].text.c_str());
    if (!current.empty())
    {
        s.blank();
        s.line(UI_YELLOW ">> %s" UI_RESET, current.c_str());
    }
}

GameSession::Progress Launcher::progressDrawer(const std::string& title, size_t firstLine)
{
    // Redraw on every new status line, otherwise at most every 50 ms: a
    // present() costs a vsync on the Wii.
    struct Throttle
    {
        size_t lines = (size_t)-1;
        uint64_t last = 0;
    };
    auto throttle = std::make_shared<Throttle>();
    return [this, title, firstLine, throttle](const std::vector<CheckLine>& lines, const std::string& current) {
        const uint64_t now = microseconds();
        if (lines.size() == throttle->lines && now - throttle->last < 50000)
            return;
        throttle->lines = lines.size();
        throttle->last = now;
        Screen s;
        header(s, title);
        drawCheckLines(s, lines, firstLine, current);
        s.footer("Please wait...");
        s.present();
    };
}

// ---------------------------------------------------------------------------
// File browser
// ---------------------------------------------------------------------------

void Launcher::openStorageRoot()
{
    root_ = gamesRoot();
    path_ = root_;
    selected_ = 0;
    refreshListing();
}

void Launcher::refreshListing()
{
    entries_.clear();
    picHere_ = false;
    std::vector<DirEntry> raw;
    if (!listDirectory(path_, raw))
    {
        message_ = "Cannot open " + path_;
        return;
    }
    for (const DirEntry& e : raw)
    {
        if (!e.isDir && hasExtension(e.name, ".pic"))
            picHere_ = true;
        if (e.isDir || hasExtension(e.name, ".chd"))
        {
            BrowserEntry b;
            b.name = e.name;
            b.path = path_ + e.name + (e.isDir ? std::string(1, pathSeparator()) : std::string());
            b.isDir = e.isDir;
            b.size = e.size;
            entries_.push_back(b);
        }
    }
    std::sort(entries_.begin(), entries_.end(), [](const BrowserEntry& a, const BrowserEntry& b) {
        if (a.isDir != b.isDir)
            return a.isDir;
        return toLower(a.name) < toLower(b.name);
    });
    if (selected_ >= (int)entries_.size())
        selected_ = 0;
}

State Launcher::browser()
{
    const bool multiStorage = storageCount() > 1;
    for (;;)
    {
        Screen s;
        s.blank();
        s.line(UI_WHITE "%s" UI_RESET " - %s", IDAS3_PORT_NAME, IDAS3_PORT_VERSION);
        s.line("Current directory: %s", path_.c_str());
        s.line("Select gds-0033.chd (with 317-0384-com.pic in the same folder):");
        s.blank();

        // Footer first, so the list knows how many rows it has.
        const int footerLines = 8;
        const int perPage = std::max(3, s.rows() - 5 - footerLines - 1);
        const int count = (int)entries_.size();
        const int pages = std::max(1, (count + perPage - 1) / perPage);
        const int page = count ? selected_ / perPage : 0;

        if (count == 0)
            s.line("  <<NO CHD FILE OR FOLDER HERE>>");
        for (int i = page * perPage; i < count && i < (page + 1) * perPage; i++)
        {
            const BrowserEntry& e = entries_[i];
            const char* cursor = i == selected_ ? UI_YELLOW "> " : "  ";
            if (e.isDir)
                s.line("%s[DIR] %s" UI_RESET, cursor, e.name.c_str());
            else
            {
                const std::string size = formatSize(e.size);
                const int pad = std::max(1, s.cols() - 4 - (int)e.name.size() - (int)size.size());
                s.line("%s%s%*s%s" UI_RESET, cursor, e.name.c_str(), pad, "", size.c_str());
            }
        }

        s.footer("--- Page %02d/%02d ---", page + 1, pages);
        s.footer("%s", message_.c_str());
        s.footer("Console detected : %s", platform::name());
        s.footer("Security PIC in this folder: %s",
                 picHere_ ? UI_GREEN "found" UI_RESET : UI_YELLOW "not found" UI_RESET);
        s.footer("Log: %s", log::path());
        if (multiStorage)
        {
            const int other = currentStorage() == 0 ? 1 : 0;
            s.footer("Storage: [%s]  (%s: Switch to %s)", storageLabel(currentStorage()), buttonLabel(BTN_2),
                     storageLabel(other));
        }
        else
        {
            s.footer("Folder: [%s]  (start with --dir <path> to change)", storageLabel(currentStorage()));
        }
        s.footer("%s: Select | %s: Back | %s: Pages | %s: Exit", buttonLabel(BTN_A), buttonLabel(BTN_B),
                 buttonLabel(BTN_LEFT | BTN_RIGHT), buttonLabel(BTN_EXIT));
        s.present();

        const uint32_t b = pollButtons(30);
        if (b & (BTN_EXIT | BTN_HOME) || exitRequested())
            return State::Quit;
        if (b)
            message_.clear();

        if (b & BTN_UP)
            selected_ = std::max(0, selected_ - 1);
        else if (b & BTN_DOWN)
            selected_ = std::min(std::max(0, count - 1), selected_ + 1);
        else if (b & BTN_LEFT)
            selected_ = std::max(0, (page - 1) * perPage);
        else if (b & BTN_RIGHT)
        {
            if (page + 1 < pages)
                selected_ = (page + 1) * perPage;
        }
        else if (b & BTN_A)
        {
            if (count == 0)
                continue;
            const BrowserEntry e = entries_[selected_];
            if (e.isDir)
            {
                path_ = e.path;
                selected_ = 0;
                refreshListing();
            }
            else
            {
                chdPath_ = e.path;
                needCheck_ = true;
                return State::Check;
            }
        }
        else if (b & BTN_B)
        {
            const std::string parent = parentOf(path_);
            if (!parent.empty())
            {
                // Re-select the folder we came from.
                const std::string came = path_;
                path_ = parent;
                selected_ = 0;
                refreshListing();
                for (int i = 0; i < (int)entries_.size(); i++)
                    if (entries_[i].path == came)
                        selected_ = i;
            }
        }
        else if ((b & BTN_2) && multiStorage)
        {
            const int other = currentStorage() == 0 ? 1 : 0;
            Screen w;
            header(w, std::string("Switching to ") + storageLabel(other) + "...");
            w.present();
            if (selectStorage(other))
                openStorageRoot();
            else
                message_ = std::string("ERROR: ") + storageLabel(other) + " is not available.";
        }
    }
}

// ---------------------------------------------------------------------------
// Game check
// ---------------------------------------------------------------------------

State Launcher::check()
{
    const std::string title = "Checking " + pathFileName(chdPath_);
    if (needCheck_)
        session_.check(chdPath_, progressDrawer(title, 0));
    needCheck_ = false;
    const bool ok = session_.checked();

    for (;;)
    {
        Screen s;
        header(s, title);
        drawCheckLines(s, session_.lines(), 0, std::string());
        if (!ok)
        {
            s.blank();
            s.line("The game cannot be prepared. Details are in the log file.");
        }
        if (ok)
            s.footer("%s: Continue | %s: Back to file list | %s: Exit", buttonLabel(BTN_A), buttonLabel(BTN_B),
                     buttonLabel(BTN_EXIT));
        else
            s.footer("%s: Retry | %s: Back to file list | %s: Exit", buttonLabel(BTN_A), buttonLabel(BTN_B),
                     buttonLabel(BTN_EXIT));
        s.present();

        const uint32_t b = pollButtons(30);
        if (b & BTN_EXIT || exitRequested())
            return State::Quit;
        if (b & BTN_B)
        {
            session_.close();
            return State::Browser;
        }
        if (b & BTN_A)
        {
            needCheck_ = !ok;   // retry after a failure
            return ok ? State::Options : State::Check;
        }
    }
}

// ---------------------------------------------------------------------------
// Launch options
// ---------------------------------------------------------------------------

State Launcher::options()
{
    // The BIOS service library: looked for next to the CHD, then in the
    // games folder.
    if (!biosSearched_)
    {
        biosSearched_ = true;
        biosError_.clear();
        haveBios_ = findBios({pathDirectory(chdPath_), gamesRoot()}, bios_, biosError_);
        log::write("[bios] %s\n", haveBios_ ? bios_.source.c_str() : biosError_.c_str());
    }
    for (;;)
    {
        // Rows of the current page: the CPU choice (-1) heads the first one.
        int rows[kCoreOptionCount + 1];
        int rowCount = 0;
        if (optionPage_ == PAGE_GENERAL)
            rows[rowCount++] = -1;
        for (int i = 0; i < kCoreOptionCount; i++)
            if (kCoreOptions[i].page == optionPage_)
                rows[rowCount++] = i;
        if (optionRow_ >= rowCount)
            optionRow_ = rowCount - 1;

        Screen s;
        header(s, "LAUNCH - " + session_.header().displayTitle() + "   [page " + std::to_string(optionPage_ + 1) + "/" +
                      std::to_string(kCoreOptionPages) + ": " + kCoreOptionPageNames[optionPage_] + "]");
        for (int r = 0; r < rowCount; r++)
        {
            const char* cursor = r == optionRow_ ? UI_YELLOW ">" : " ";
            if (rows[r] < 0)
                s.line("%s SH-4 CPU       : [< %-17s >]" UI_RESET, cursor, run_.dynarec ? "RECOMPILER" : "INTERPRETER");
            else
            {
                const CoreOption& o = kCoreOptions[rows[r]];
                s.line("%s %s : [< %-17s >]" UI_RESET, cursor, o.label, o.names[run_.presets[rows[r]]]);
            }
        }
        const int sel = rows[optionRow_];
        s.line(UI_CYAN "  %s" UI_RESET, sel < 0 ? "recompiler: fast; interpreter: slow, for comparisons"
                                                : kCoreOptions[sel].hint);
        if (optionPage_ == PAGE_GENERAL)
        {
            if (haveBios_)
                s.line(UI_GREEN "[ OK ]" UI_RESET " BIOS service library: %s", bios_.source.c_str());
            else
                s.line(UI_RED "[FAIL]" UI_RESET " %s (MAME's naomi.zip works)", biosError_.c_str());
            for (const std::string& line : gameControlsHelp())
                s.line("%s", line.c_str());
        }
        s.footer("%s: Option | %s: Change | %s/%s: Page", buttonLabel(BTN_UP | BTN_DOWN),
                 buttonLabel(BTN_LEFT | BTN_RIGHT), buttonLabel(BTN_1), buttonLabel(BTN_2));
        if (haveBios_)
            s.footer("%s: Launch | %s: Back | %s: Exit", buttonLabel(BTN_A), buttonLabel(BTN_B), buttonLabel(BTN_EXIT));
        else
            s.footer("%s: Search again | %s: Back | %s: Exit", buttonLabel(BTN_A), buttonLabel(BTN_B),
                     buttonLabel(BTN_EXIT));
        s.present();

        const uint32_t b = pollButtons(30);
        if (b & BTN_EXIT || exitRequested())
            return State::Quit;
        if (b & (BTN_1 | BTN_2))
        {
            optionPage_ = (optionPage_ + ((b & BTN_1) ? kCoreOptionPages - 1 : 1)) % kCoreOptionPages;
            optionRow_ = 0;
        }
        else if (b & BTN_UP)
            optionRow_ = (optionRow_ + rowCount - 1) % rowCount;
        else if (b & BTN_DOWN)
            optionRow_ = (optionRow_ + 1) % rowCount;
        else if (b & (BTN_LEFT | BTN_RIGHT))
        {
            if (sel < 0)
                run_.dynarec = !run_.dynarec;
            else
            {
                const int n = kCoreOptions[sel].count;
                int& v = run_.presets[sel];
                v = (v + ((b & BTN_LEFT) ? n - 1 : 1)) % n;
            }
        }
        else if (b & BTN_A)
        {
            if (haveBios_)
            {
                saveOptions();
                return State::Launch;
            }
            biosSearched_ = false;
            return State::Options;
        }
        else if (b & BTN_B)
            return State::Check;
    }
}

// ---------------------------------------------------------------------------
// Launch
// ---------------------------------------------------------------------------

State Launcher::launch()
{
    const std::string title = "LAUNCH - " + session_.header().displayTitle();
    std::string error;
#if defined(GEKKO)
    {
        Screen s;
        header(s, title);
        s.line("  Starting the NAOMI 2 core...");
        s.present();
    }
    if (runGame(session_, bios_, run_, error))
        return State::Quit;   // the core owns the machine now: back to the loader
#else
    error = "On Windows the game runs in the headless runner: idas3wii_run --chd <file>";
#endif
    for (;;)
    {
        Screen s;
        header(s, title);
        s.line(UI_RED "[FAIL]" UI_RESET " %s", error.c_str());
        s.footer("%s: Back | %s: Exit", buttonLabel(BTN_B), buttonLabel(BTN_EXIT));
        s.present();
        const uint32_t b = pollButtons(30);
        if (b & BTN_EXIT || exitRequested())
            return State::Quit;
        if (b & BTN_B)
            return State::Options;
    }
}

// The launch options chosen last time: options.txt in the app folder, one
// "LABEL=VALUE" line each, matched by name (rows come and go between
// versions; a positional list would shift). Saved when a game is launched
// from the options pages.
static std::string optionLabel(const char* label)
{
    std::string s = label;
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    return s;
}

void Launcher::loadOptions()
{
    FILE* f = fopen((appDir() + "options.txt").c_str(), "r");
    if (!f)
        return;
    char line[160];
    int restored = 0;
    while (fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = 0;
        char* eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        const std::string label = line, value = eq + 1;
        if (label == "SH-4 CPU")
        {
            run_.dynarec = value != "INTERPRETER";
            restored++;
            continue;
        }
        for (int i = 0; i < kCoreOptionCount; i++)
            if (optionLabel(kCoreOptions[i].label) == label)
                for (int k = 0; k < kCoreOptions[i].count; k++)
                    if (value == kCoreOptions[i].names[k])
                    {
                        run_.presets[i] = k;
                        restored++;
                    }
    }
    fclose(f);
    log::write("[boot] options.txt: %d launch options restored\n", restored);
}

void Launcher::saveOptions() const
{
    FILE* f = fopen((appDir() + "options.txt").c_str(), "w");
    if (!f)
        return;
    fprintf(f, "SH-4 CPU=%s\n", run_.dynarec ? "RECOMPILER" : "INTERPRETER");
    for (int i = 0; i < kCoreOptionCount; i++)
        fprintf(f, "%s=%s\n", optionLabel(kCoreOptions[i].label).c_str(), kCoreOptions[i].names[run_.presets[i]]);
    fclose(f);
}

// Test runs without a pad: "autolaunch.txt" in the games folder, holding
// "<chd file name> [rec|int] [preset values in CoreOptionId order]", goes
// straight from the check to the launch.
State Launcher::autoLaunch()
{
    FILE* f = fopen((root_ + "autolaunch.txt").c_str(), "r");
    if (!f)
        return State::Browser;
    char name[256] = {}, cpu[16] = {};
    const int n = fscanf(f, "%255s %15s", name, cpu);
    for (int i = 0, v; i < kCoreOptionCount && fscanf(f, "%d", &v) == 1; i++)
        if (v >= 0 && v < kCoreOptions[i].count)
            run_.presets[i] = v;
    fclose(f);
    if (n < 1)
        return State::Browser;
    run_.dynarec = n < 2 || strcmp(cpu, "int") != 0;
    chdPath_ = root_ + name;
    log::write("[boot] autolaunch.txt: %s, %s\n", chdPath_.c_str(), run_.dynarec ? "recompiler" : "interpreter");
    session_.check(chdPath_, progressDrawer("Checking " + pathFileName(chdPath_), 0));
    needCheck_ = false;
    if (!session_.checked())
        return State::Check;
    biosSearched_ = true;
    haveBios_ = findBios({pathDirectory(chdPath_), gamesRoot()}, bios_, biosError_);
    log::write("[bios] %s\n", haveBios_ ? bios_.source.c_str() : biosError_.c_str());
    return haveBios_ ? State::Launch : State::Options;
}

int Launcher::run()
{
    log::write("[boot] %s %s on %s\n", IDAS3_PORT_NAME, IDAS3_PORT_VERSION, platform::name());
    log::write("[boot] app folder %s, games folder %s\n", appDir().c_str(), gamesRoot().c_str());
    log::write("[boot] %s\n", memoryReport().c_str());

    openStorageRoot();
    loadOptions();
    State state = autoLaunch();
    while (state != State::Quit)
    {
        switch (state)
        {
        case State::Browser:
            state = browser();
            if (state == State::Browser)
                refreshListing();
            break;
        case State::Check:
            state = check();
            if (state == State::Browser)
                refreshListing();
            break;
        case State::Options:
            state = options();
            break;
        case State::Launch:
            state = launch();
            if (state == State::Browser)
                refreshListing();
            break;
        case State::Quit:
            break;
        }
    }
    session_.close();
    log::write("[boot] exit\n");
    return 0;
}

} // namespace

int runLauncher()
{
    Launcher l;
    return l.run();
}

} // namespace ui
} // namespace idas3
