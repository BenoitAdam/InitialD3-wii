// Platform layer: the only code that differs between the Wii and Windows.
//
// Both backends expose the same text console (the launcher is drawn with
// plain text, like nullDC4Wii's menus), the same logical buttons, the same
// storage view and the same large-allocation primitive, so the menu and the
// whole game-loading pipeline are shared and behave identically.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace idas3 {
namespace platform {

// Logical buttons, Wiimote naming (nullDC4Wii convention).
enum Button : uint32_t
{
    BTN_UP = 1u << 0,
    BTN_DOWN = 1u << 1,
    BTN_LEFT = 1u << 2,
    BTN_RIGHT = 1u << 3,
    BTN_A = 1u << 4,
    BTN_B = 1u << 5,
    BTN_1 = 1u << 6,
    BTN_2 = 1u << 7,
    BTN_PLUS = 1u << 8,
    BTN_MINUS = 1u << 9,
    BTN_HOME = 1u << 10,
    BTN_EXIT = 1u << 11,   // synthesized: (-)+(+) / L+R+Z on Wii, Q / window close on Windows
};

bool init(int argc, char** argv);
void shutdown();

// While the game runs nothing reads the console: send stdout/stderr (the
// emulator core's printf diagnostics) to the log file instead, and back.
void captureStdout();
void releaseStdout();

// "Wii", "Wii U (vWii)" or "Windows".
const char* name();

// True once the user asked to leave (power/reset button, closed window...).
bool exitRequested();

// The console's POWER button was pressed (the exit then powers off).
bool powerOffRequested();

// ---- Text console --------------------------------------------------------
int consoleCols();
int consoleRows();
// Replaces the whole screen with `text` ('\n' separated, already fitted to
// consoleCols() x consoleRows()).
void present(const std::string& text);

// ---- Input ---------------------------------------------------------------
// Buttons newly pressed since the previous call. Blocks for at most one
// frame on the Wii (vsync) and up to `waitMs` on Windows.
uint32_t pollButtons(int waitMs);
uint32_t heldButtons();
// What the user physically presses for a logical button ("A" / "Enter").
const char* buttonLabel(uint32_t button);

// ---- Game controls (while the game runs) ----------------------------------
struct GameControls
{
    float steer = 0.f;      // -1 full left .. +1 full right
    float accel = 0.f;      // 0 released .. 1 floored
    float brake = 0.f;
    bool shiftUp = false, shiftDown = false, view = false;
    bool start = false, coin = false, test = false, service = false;
    bool quit = false;      // leave the game
};
// Reads every connected pad and merges them.
void readGameControls(GameControls& out);
// 2 PLAYERS: the second controller slot (GameCube port 2, Wiimote 2) in p2,
// every other one merged in p1.
void readGameControlsTwo(GameControls& p1, GameControls& p2);
// The controllers seen at the last read, for the log: bits 0-3 GameCube ports
// 1-4, bits 4-7 Wiimotes 1-4, bits 8-15 each Wiimote's expansion (2 bits:
// 0 none, 1 Nunchuk, 2 Classic, 3 other).
uint32_t controllersSeen();
// How the controls are mapped, one line per controller type.
std::vector<std::string> gameControlsHelp();

// ---- Storage -------------------------------------------------------------
struct DirEntry
{
    std::string name;
    bool isDir = false;
    uint64_t size = 0;
};

bool listDirectory(const std::string& path, std::vector<DirEntry>& out);
bool fileExists(const std::string& path);
uint64_t fileSize(const std::string& path);
char pathSeparator();

// Storage devices the browser can switch between (SD/USB on the Wii, the
// game folder on Windows).
int storageCount();
int currentStorage();
const char* storageLabel(int index);
// Mounts if needed; false when the device is not available.
bool selectStorage(int index);
// Default games folder of the current storage device (with trailing separator).
std::string gamesRoot();

// Folder of the executable / boot.dol (with trailing separator).
std::string appDir();
std::string logPath();

// ---- Memory --------------------------------------------------------------
// Large, long-lived guest buffers (NAOMI RAM/VRAM/...). On the Wii they are
// carved from the top of MEM2; release them in reverse order of allocation.
void* allocLarge(size_t bytes);
void freeLarge(void* p, size_t bytes);
// Free memory available to allocLarge(), in bytes.
size_t largeFreeBytes();
// One line, e.g. "MEM1 12.3 MB free, MEM2 45.6 MB free".
std::string memoryReport();

// ---- Time ----------------------------------------------------------------
uint64_t microseconds();
void sleepMs(int ms);

} // namespace platform
} // namespace idas3
