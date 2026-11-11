// Windows backend: a console window that draws the same text screens as the
// Wii build (same 77x28 layout), keyboard input mapped onto the Wiimote
// buttons, Win32 file listing and plain heap allocations.
#if defined(_WIN32)

#include "platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

namespace idas3 {
namespace platform {

// Same visible area as the Wii console (640x480 XFB, 8x16 font, 20 px margin).
static constexpr int kCols = 77;
static constexpr int kRows = 28;

static HANDLE s_out = INVALID_HANDLE_VALUE;
static HANDLE s_in = INVALID_HANDLE_VALUE;
static DWORD s_oldOutMode = 0, s_oldInMode = 0;
static UINT s_oldCodePage = 0;
static volatile LONG s_exit = 0;
static std::string s_lastScreen;
static std::string s_appDir;
static std::string s_gamesRoot;
static uint32_t s_held = 0;
static LARGE_INTEGER s_freq;

// Test harness: --keys "DOWN,A,WAIT,B" replaces the keyboard with a script
// (one token per poll, EXIT once it runs out) and --dump-screens <file>
// appends every distinct screen to a text file.
static std::vector<uint32_t> s_script;
static size_t s_scriptPos = 0;
static bool s_scripted = false;
static FILE* s_dump = nullptr;

static uint32_t tokenToButton(const std::string& t)
{
    static const struct { const char* name; uint32_t button; } kNames[] = {
        {"UP", BTN_UP}, {"DOWN", BTN_DOWN}, {"LEFT", BTN_LEFT}, {"RIGHT", BTN_RIGHT},
        {"A", BTN_A}, {"B", BTN_B}, {"1", BTN_1}, {"2", BTN_2}, {"PLUS", BTN_PLUS},
        {"MINUS", BTN_MINUS}, {"HOME", BTN_HOME}, {"EXIT", BTN_EXIT}, {"WAIT", 0},
    };
    for (const auto& k : kNames)
        if (t == k.name)
            return k.button;
    return 0;
}

static void parseScript(const char* s)
{
    std::string token;
    for (const char* p = s;; p++)
    {
        if (*p == ',' || *p == '\0')
        {
            if (!token.empty())
                s_script.push_back(tokenToButton(token));
            token.clear();
            if (*p == '\0')
                break;
        }
        else if (*p != ' ')
        {
            token += (char)toupper((unsigned char)*p);
        }
    }
    s_scripted = true;
}

static std::wstring widen(const std::string& s)
{
    if (s.empty())
        return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string narrow(const wchar_t* w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return std::string();
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

static bool isDirectory(const std::string& path)
{
    DWORD a = GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string withSlash(std::string p)
{
    if (!p.empty() && p.back() != '\\' && p.back() != '/')
        p += '\\';
    return p;
}

static BOOL WINAPI ctrlHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT)
    {
        InterlockedExchange(&s_exit, 1);
        return TRUE;
    }
    return FALSE;
}

bool init(int argc, char** argv)
{
    QueryPerformanceFrequency(&s_freq);

    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
    {
        std::string p = narrow(exe);
        size_t s = p.find_last_of("\\/");
        s_appDir = s == std::string::npos ? std::string(".\\") : p.substr(0, s + 1);
    }
    else
    {
        s_appDir = ".\\";
    }

    // Games folder: --dir <path>, or the folder of a .chd given on the
    // command line (drag and drop onto the exe), else "game files" next to
    // the exe (the original Windows release layout), else the exe folder.
    for (int i = 1; i < argc; i++)
    {
        const size_t len = strlen(argv[i]);
        if (len > 4 && _stricmp(argv[i] + len - 4, ".chd") == 0)
        {
            std::string p = argv[i];
            const size_t s = p.find_last_of("\\/");
            if (s != std::string::npos)
                s_gamesRoot = p.substr(0, s + 1);
        }
    }
    for (int i = 1; i + 1 < argc; i++)
    {
        if (!strcmp(argv[i], "--dir"))
            s_gamesRoot = withSlash(argv[i + 1]);
        else if (!strcmp(argv[i], "--keys"))
            parseScript(argv[i + 1]);
        else if (!strcmp(argv[i], "--dump-screens"))
            s_dump = fopen(argv[i + 1], "w");
    }
    if (s_gamesRoot.empty() || !isDirectory(s_gamesRoot))
    {
        if (isDirectory(s_appDir + "game files"))
            s_gamesRoot = s_appDir + "game files\\";
        else
            s_gamesRoot = s_appDir;
    }

    s_out = GetStdHandle(STD_OUTPUT_HANDLE);
    s_in = GetStdHandle(STD_INPUT_HANDLE);
    GetConsoleMode(s_out, &s_oldOutMode);
    GetConsoleMode(s_in, &s_oldInMode);
    SetConsoleMode(s_out, s_oldOutMode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleMode(s_in, ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT);
    s_oldCodePage = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(ctrlHandler, TRUE);
    SetConsoleTitleW(L"Initial D Arcade Stage 3 - Wii port (Windows build)");

    // Size the window to the Wii console layout when the host allows it
    // (classic conhost does; Windows Terminal ignores it, which is fine).
    SMALL_RECT tiny = {0, 0, 1, 1};
    SetConsoleWindowInfo(s_out, TRUE, &tiny);
    COORD size = {(SHORT)(kCols + 1), (SHORT)(kRows + 1)};
    SetConsoleScreenBufferSize(s_out, size);
    SMALL_RECT win = {0, 0, (SHORT)kCols, (SHORT)kRows};
    SetConsoleWindowInfo(s_out, TRUE, &win);

    fputs("\x1b[?25l\x1b[2J\x1b[H", stdout);   // hide cursor, clear
    fflush(stdout);
    return true;
}

void captureStdout() {}
bool powerOffRequested()
{
    return false;
}
void releaseStdout() {}

void shutdown()
{
    fputs("\x1b[0m\x1b[2J\x1b[H\x1b[?25h", stdout);
    fflush(stdout);
    if (s_out != INVALID_HANDLE_VALUE)
        SetConsoleMode(s_out, s_oldOutMode);
    if (s_in != INVALID_HANDLE_VALUE)
        SetConsoleMode(s_in, s_oldInMode);
    if (s_oldCodePage)
        SetConsoleOutputCP(s_oldCodePage);
    if (s_dump)
    {
        fclose(s_dump);
        s_dump = nullptr;
    }
}

const char* name()
{
    return "Windows";
}

bool exitRequested()
{
    return s_exit != 0;
}

int consoleCols()
{
    return kCols;
}

int consoleRows()
{
    return kRows;
}

void present(const std::string& text)
{
    // Only redraw on change: the UI presents every frame (like the Wii's
    // double-buffered console) but rewriting an unchanged console flickers.
    if (text == s_lastScreen)
        return;
    s_lastScreen = text;

    if (s_dump)
    {
        // Strip colour escapes so the dump reads like the screen.
        std::string plain;
        for (size_t i = 0; i < text.size(); i++)
        {
            if (text[i] == '\x1b')
            {
                while (i < text.size() && !((text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= 'a' && text[i] <= 'z')))
                    i++;
                continue;
            }
            plain += text[i];
        }
        fprintf(s_dump, "=============================================================================\n%s\n",
                plain.c_str());
        fflush(s_dump);
    }

    std::string out = "\x1b[H";
    size_t start = 0;
    while (start <= text.size())
    {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        out.append(text, start, end - start);
        out += "\x1b[0m\x1b[K";
        if (end >= text.size())
            break;
        out += "\r\n";
        start = end + 1;
    }
    out += "\x1b[J";
    DWORD written = 0;
    WriteConsoleA(s_out, out.data(), (DWORD)out.size(), &written, nullptr);
}

static uint32_t mapKey(WORD vk, char ch)
{
    switch (vk)
    {
    case VK_UP: return BTN_UP;
    case VK_DOWN: return BTN_DOWN;
    case VK_LEFT: return BTN_LEFT;
    case VK_RIGHT: return BTN_RIGHT;
    case VK_PRIOR: return BTN_LEFT;    // Page Up/Down: previous/next page
    case VK_NEXT: return BTN_RIGHT;
    case VK_RETURN:
    case VK_SPACE: return BTN_A;
    case VK_BACK:
    case VK_ESCAPE: return BTN_B;
    case VK_HOME: return BTN_HOME;
    case VK_F10: return BTN_EXIT;
    case VK_ADD:
    case VK_OEM_PLUS: return BTN_PLUS;
    case VK_SUBTRACT:
    case VK_OEM_MINUS: return BTN_MINUS;
    default: break;
    }
    switch (ch)
    {
    case '1': return BTN_1;
    case '2': return BTN_2;
    case 'q':
    case 'Q': return BTN_EXIT;
    default: break;
    }
    return 0;
}

uint32_t pollButtons(int waitMs)
{
    uint32_t pressed = 0;
    if (s_scripted)
    {
        if (s_scriptPos >= s_script.size())
            return BTN_EXIT;
        return s_script[s_scriptPos++];
    }
    if (s_in == INVALID_HANDLE_VALUE)
    {
        Sleep(waitMs);
        return 0;
    }
    if (WaitForSingleObject(s_in, (DWORD)(waitMs < 0 ? 0 : waitMs)) != WAIT_OBJECT_0)
        return 0;

    DWORD available = 0;
    while (GetNumberOfConsoleInputEvents(s_in, &available) && available > 0)
    {
        INPUT_RECORD rec[32];
        DWORD read = 0;
        if (!ReadConsoleInputA(s_in, rec, 32, &read))
            break;
        for (DWORD i = 0; i < read; i++)
        {
            if (rec[i].EventType != KEY_EVENT)
                continue;
            const KEY_EVENT_RECORD& k = rec[i].Event.KeyEvent;
            const uint32_t b = mapKey(k.wVirtualKeyCode, k.uChar.AsciiChar);
            if (k.bKeyDown)
            {
                pressed |= b;
                s_held |= b;
            }
            else
            {
                s_held &= ~b;
            }
        }
    }
    if (s_exit)
        pressed |= BTN_EXIT;
    return pressed;
}

uint32_t heldButtons()
{
    return s_held;
}

void readGameControls(GameControls& c)
{
    // The Windows launcher does not run the game (idas3wii_run does,
    // headless): neutral controls, quit when the window is closing.
    c = GameControls();
    c.quit = exitRequested();
}

void readGameControlsTwo(GameControls& p1, GameControls& p2)
{
    readGameControls(p1);
    p2 = GameControls();
}

uint32_t controllersSeen()
{
    return 0;
}

void setPadLayout(int)
{
}

std::vector<std::string> gameControlsHelp(int)
{
    return {"Windows: the game runs in the headless runner (idas3wii_run)."};
}

const char* buttonLabel(uint32_t button)
{
    switch (button)
    {
    case BTN_A: return "Enter";
    case BTN_B: return "Backspace";
    case BTN_1: return "1";
    case BTN_2: return "2";
    case BTN_MINUS: return "-";
    case BTN_PLUS: return "+";
    case BTN_EXIT: return "Q";
    case BTN_LEFT | BTN_RIGHT: return "Left/Right";
    case BTN_UP | BTN_DOWN: return "Up/Down";
    default: return "?";
    }
}

bool listDirectory(const std::string& path, std::vector<DirEntry>& out)
{
    out.clear();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(widen(withSlash(path) + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    do
    {
        std::string n = narrow(fd.cFileName);
        if (n == "." || n == "..")
            continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
            continue;
        DirEntry e;
        e.name = n;
        e.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        out.push_back(e);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

bool fileExists(const std::string& path)
{
    DWORD a = GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

uint64_t fileSize(const std::string& path)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &d))
        return 0;
    return ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

char pathSeparator()
{
    return '\\';
}

int storageCount()
{
    return 1;
}

int currentStorage()
{
    return 0;
}

const char* storageLabel(int)
{
    return "GAME FOLDER";
}

bool selectStorage(int index)
{
    return index == 0;
}

std::string gamesRoot()
{
    return s_gamesRoot;
}

std::string appDir()
{
    return s_appDir;
}

std::string logPath()
{
    return s_appDir + "idas3wii_log.txt";
}

void* allocLarge(size_t bytes)
{
    return _aligned_malloc(bytes, 64);
}

void freeLarge(void* p, size_t)
{
    _aligned_free(p);
}

size_t largeFreeBytes()
{
    MEMORYSTATUSEX m;
    m.dwLength = sizeof(m);
    if (!GlobalMemoryStatusEx(&m))
        return 0;
    return (size_t)(m.ullAvailPhys < m.ullAvailVirtual ? m.ullAvailPhys : m.ullAvailVirtual);
}

std::string memoryReport()
{
    MEMORYSTATUSEX m;
    m.dwLength = sizeof(m);
    char buf[96];
    if (GlobalMemoryStatusEx(&m))
        snprintf(buf, sizeof(buf), "PC %.0f MB physical free", (double)m.ullAvailPhys / (1024.0 * 1024.0));
    else
        snprintf(buf, sizeof(buf), "PC memory unknown");
    return buf;
}

uint64_t microseconds()
{
    if (s_freq.QuadPart == 0)
        QueryPerformanceFrequency(&s_freq);   // headless (CLI) use skips init()
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const uint64_t ticks = (uint64_t)c.QuadPart, freq = (uint64_t)s_freq.QuadPart;
    return ticks / freq * 1000000ull + ticks % freq * 1000000ull / freq;
}

void sleepMs(int ms)
{
    Sleep((DWORD)ms);
}

} // namespace platform
} // namespace idas3

#endif // _WIN32
