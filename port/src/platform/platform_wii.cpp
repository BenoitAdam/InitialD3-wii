// Wii backend (libogc): double-buffered text console on the XFB, Wiimote /
// Classic Controller / GameCube pad input, SD and USB through libfat, and a
// MEM2 allocator for the big NAOMI guest buffers. The console, storage and
// controller handling follow nullDC4Wii's wii/main.cpp.
#if defined(GEKKO)

#include "platform.h"

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/usbstorage.h>
#include <wiiuse/wpad.h>
#include <fat.h>

#include <dirent.h>
#include <malloc.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/iosupport.h>
#include "../core/log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace idas3 {
namespace platform {

// ---------------------------------------------------------------------------
// Video / console
// ---------------------------------------------------------------------------

static GXRModeObj* s_rmode = nullptr;
static void* s_xfb[2] = {nullptr, nullptr};
static int s_fb = 0;
static int s_cols = 76;
static int s_rows = 27;
static bool s_isWiiU = false;
static volatile bool s_powerPressed = false;
static volatile bool s_resetPressed = false;

static void consoleOnBackBuffer()
{
    console_init(s_xfb[s_fb], 20, 20, s_rmode->fbWidth, s_rmode->xfbHeight,
                 s_rmode->fbWidth * VI_DISPLAY_PIX_SZ);
}

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------

static bool s_sdMounted = false;
static bool s_usbMounted = false;
static int s_storage = 0;   // 0 = SD, 1 = USB
static char s_appDir[256] = "";
static char s_launchDevice[16] = "";

static bool dirExists(const char* path)
{
    if (!path || !path[0])
        return false;
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 1 && tmp[len - 1] == '/' && tmp[len - 2] != ':')
        tmp[len - 1] = '\0';
    struct stat st;
    if (stat(tmp, &st) == 0)
        return S_ISDIR(st.st_mode);
    DIR* d = opendir(path);
    if (d)
    {
        closedir(d);
        return true;
    }
    return false;
}

static bool mountUsb(int timeoutMs)
{
    if (s_usbMounted || dirExists("usb:/"))
        return s_usbMounted = true;
    for (int waited = 0; waited < timeoutMs; waited += 100)
    {
        if (USBStorage_Initialize() == 0 && (fatMountSimple("usb", &__io_usbstorage) || dirExists("usb:/")))
            return s_usbMounted = true;
        usleep(100 * 1000);
    }
    return false;
}

static void parseLaunchPath()
{
    if (!__system_argv || __system_argv->argvMagic != ARGV_MAGIC || __system_argv->argc < 1 ||
        !__system_argv->argv || !__system_argv->argv[0])
        return;
    const char* a0 = __system_argv->argv[0];
    const char* colon = strchr(a0, ':');
    const char* slash = strrchr(a0, '/');
    if (!colon || !slash || slash < colon)
        return;
    size_t devLen = (size_t)(colon - a0);
    size_t dirLen = (size_t)(slash - a0) + 1;
    if (devLen == 0 || devLen >= sizeof(s_launchDevice) || dirLen >= sizeof(s_appDir))
        return;
    memcpy(s_launchDevice, a0, devLen);
    s_launchDevice[devLen] = '\0';
    memcpy(s_appDir, a0, dirLen);
    s_appDir[dirLen] = '\0';
}

// ---------------------------------------------------------------------------
// vWii detection (two of nullDC4Wii's three signals; the GamePad probe needs
// libwiidrc, which this launcher does not link).
// ---------------------------------------------------------------------------

#define HW_AHBPROT_REG (*(vu32*)0xCD800064)
#define WIIU_SIG_REG (*(vu32*)0xCD8005A0)

static bool detectWiiU()
{
    if (HW_AHBPROT_REG == 0xFFFFFFFF && (WIIU_SIG_REG >> 16) == 0xCAFE)
        return true;

    const u64 ios58 = 0x0000000100000000ULL | 58;
    u32 viewSize = 0;
    if (ES_GetTMDViewSize(ios58, &viewSize) < 0 || viewSize == 0)
        return false;
    tmd_view* view = (tmd_view*)memalign(32, viewSize);
    if (!view)
        return false;
    bool wiiu = ES_GetTMDView(ios58, view, viewSize) >= 0 && view->title_version == 6432;
    free(view);
    return wiiu;
}

static void onPower()
{
    s_powerPressed = true;
}

static void onReset(u32, void*)
{
    s_resetPressed = true;
}

static void onWiimotePower(s32)
{
    s_powerPressed = true;
}

bool init(int, char**)
{
    VIDEO_Init();
    PAD_Init();
    WPAD_Init();

    s_rmode = VIDEO_GetPreferredMode(nullptr);
    s_xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(s_rmode));
    s_xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(s_rmode));
    s_fb = 0;
    consoleOnBackBuffer();
    VIDEO_Configure(s_rmode);
    VIDEO_SetNextFramebuffer(s_xfb[s_fb]);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (s_rmode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();

    int cols = 0, rows = 0;
    CON_GetMetrics(&cols, &rows);
    // One column of slack: the libogc console wraps instead of clipping.
    if (cols > 1)
        s_cols = cols - 1;
    if (rows > 1)
        s_rows = rows;

    SYS_SetPowerCallback(onPower);
    SYS_SetResetCallback(onReset);
    WPAD_SetPowerButtonCallback(onWiimotePower);

    s_isWiiU = detectWiiU();

    printf("\n  Initial D Arcade Stage 3 - Wii port\n\n  Mounting storage...\n");
    VIDEO_WaitVSync();

    // Give the hardware a moment after the loader hand-off (nullDC4Wii does
    // the same), then mount everything libfat can see.
    usleep(300000);
    fatInitDefault();
    parseLaunchPath();
    s_sdMounted = dirExists("sd:/");
    const bool fromUsb = strncmp(s_launchDevice, "usb", 3) == 0;
    s_usbMounted = dirExists("usb:/");
    if (fromUsb && !s_usbMounted)
        mountUsb(10000);
    if (s_appDir[0] && dirExists(s_appDir))
        chdir(s_appDir);
    else
        s_appDir[0] = '\0';

    // Browse the device we were launched from; SD otherwise.
    s_storage = ((fromUsb || !s_sdMounted) && s_usbMounted) ? 1 : 0;
    return true;
}

// stdout/stderr -> log file (log::writeRaw) while the game runs.
static const devoptab_t* s_consoleOut = nullptr;
static const devoptab_t* s_consoleErr = nullptr;
static devoptab_t s_logOut;

static ssize_t logWrite(struct _reent*, void*, const char* ptr, size_t len)
{
    log::writeRaw(ptr, (unsigned)len);
    return (ssize_t)len;
}

void captureStdout()
{
    if (s_consoleOut)
        return;
    fflush(stdout);
    fflush(stderr);
    memset(&s_logOut, 0, sizeof(s_logOut));
    s_logOut.name = "stdout";
    s_logOut.write_r = logWrite;
    s_consoleOut = devoptab_list[STD_OUT];
    s_consoleErr = devoptab_list[STD_ERR];
    devoptab_list[STD_OUT] = &s_logOut;
    devoptab_list[STD_ERR] = &s_logOut;
}

void releaseStdout()
{
    if (!s_consoleOut)
        return;
    fflush(stdout);
    fflush(stderr);
    devoptab_list[STD_OUT] = s_consoleOut;
    devoptab_list[STD_ERR] = s_consoleErr;
    s_consoleOut = s_consoleErr = nullptr;
}

void shutdown()
{
    WPAD_Shutdown();
    if (s_powerPressed)
        SYS_ResetSystem(SYS_POWEROFF, 0, 0);
}

const char* name()
{
    return s_isWiiU ? "Wii U (vWii)" : "Wii";
}

bool powerOffRequested()
{
    return s_powerPressed;
}

bool exitRequested()
{
    return s_powerPressed || s_resetPressed;
}

int consoleCols()
{
    return s_cols;
}

int consoleRows()
{
    return s_rows;
}

void present(const std::string& text)
{
    // Draw into the back buffer, show it, then aim the console at the other
    // buffer: no tearing or flicker even though the whole screen is redrawn.
    printf("\x1b[2J\x1b[H");
    fputs(text.c_str(), stdout);
    fflush(stdout);
    VIDEO_SetNextFramebuffer(s_xfb[s_fb]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    s_fb ^= 1;
    consoleOnBackBuffer();
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

static uint32_t fromWpad(u32 w)
{
    uint32_t b = 0;
    if (w & (WPAD_BUTTON_UP | WPAD_CLASSIC_BUTTON_UP)) b |= BTN_UP;
    if (w & (WPAD_BUTTON_DOWN | WPAD_CLASSIC_BUTTON_DOWN)) b |= BTN_DOWN;
    if (w & (WPAD_BUTTON_LEFT | WPAD_CLASSIC_BUTTON_LEFT)) b |= BTN_LEFT;
    if (w & (WPAD_BUTTON_RIGHT | WPAD_CLASSIC_BUTTON_RIGHT)) b |= BTN_RIGHT;
    if (w & (WPAD_BUTTON_A | WPAD_CLASSIC_BUTTON_A)) b |= BTN_A;
    if (w & (WPAD_BUTTON_B | WPAD_CLASSIC_BUTTON_B)) b |= BTN_B;
    if (w & (WPAD_BUTTON_1 | WPAD_CLASSIC_BUTTON_Y)) b |= BTN_1;
    if (w & (WPAD_BUTTON_2 | WPAD_CLASSIC_BUTTON_X)) b |= BTN_2;
    if (w & (WPAD_BUTTON_PLUS | WPAD_CLASSIC_BUTTON_PLUS)) b |= BTN_PLUS;
    if (w & (WPAD_BUTTON_MINUS | WPAD_CLASSIC_BUTTON_MINUS)) b |= BTN_MINUS;
    if (w & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME)) b |= BTN_HOME;
    return b;
}

static uint32_t fromPad(u16 p)
{
    // Same convention as nullDC4Wii's menus: Y = 1, X = 2.
    uint32_t b = 0;
    if (p & PAD_BUTTON_UP) b |= BTN_UP;
    if (p & PAD_BUTTON_DOWN) b |= BTN_DOWN;
    if (p & PAD_BUTTON_LEFT) b |= BTN_LEFT;
    if (p & PAD_BUTTON_RIGHT) b |= BTN_RIGHT;
    if (p & PAD_BUTTON_A) b |= BTN_A;
    if (p & PAD_BUTTON_B) b |= BTN_B;
    if (p & PAD_BUTTON_Y) b |= BTN_1;
    if (p & PAD_BUTTON_X) b |= BTN_2;
    if (p & PAD_BUTTON_START) b |= BTN_PLUS;
    return b;
}

static uint32_t s_held = 0;

static bool exitCombo(uint32_t held, u16 padHeld)
{
    if ((held & BTN_PLUS) && (held & BTN_MINUS))
        return true;
    // GameCube: L + R + Z (or L + R + Start), like nullDC4Wii.
    return (padHeld & PAD_TRIGGER_L) && (padHeld & PAD_TRIGGER_R) &&
           (padHeld & (PAD_TRIGGER_Z | PAD_BUTTON_START));
}

uint32_t pollButtons(int)
{
    WPAD_ScanPads();
    PAD_ScanPads();
    uint32_t down = 0, held = 0;
    u16 padHeld = 0;
    for (int i = 0; i < 4; i++)
    {
        down |= fromWpad(WPAD_ButtonsDown(i)) | fromPad(PAD_ButtonsDown(i));
        held |= fromWpad(WPAD_ButtonsHeld(i)) | fromPad(PAD_ButtonsHeld(i));
        padHeld |= PAD_ButtonsHeld(i);
    }
    s_held = held;
    if (exitCombo(held, padHeld) || s_powerPressed || s_resetPressed)
        down |= BTN_EXIT;
    return down;
}

uint32_t heldButtons()
{
    return s_held;
}

// ---------------------------------------------------------------------------
// Game controls
// ---------------------------------------------------------------------------

static float stickX(const joystick_t& js)
{
    // wiiuse gives magnitude (0..1) and angle in degrees, 0 = up, clockwise.
    if (js.mag < 0.15f)
        return 0.f;
    float x = js.mag * sinf(js.ang * 3.14159265f / 180.f);
    return x < -1.f ? -1.f : x > 1.f ? 1.f : x;
}

static float padAxis(s8 v)
{
    // GameCube stick: about +-100 at the gate, small dead zone.
    if (v > -12 && v < 12)
        return 0.f;
    const float x = v / 100.f;
    return x < -1.f ? -1.f : x > 1.f ? 1.f : x;
}

static float padTrigger(u8 v)
{
    // GameCube analog triggers rest around 20-30 and reach ~200 at the click.
    const float x = (v - 30) / 170.f;
    return x < 0.f ? 0.f : x > 1.f ? 1.f : x;
}

// One controller slot: GameCube port i and Wiimote i (with its expansion),
// added to c.
static void addPad(int i, GameControls& c)
{
    // GameCube pad: stick steers, R/L analog triggers are the pedals.
    const u16 p = PAD_ButtonsHeld(i);
    if (padAxis(PAD_StickX(i)) != 0.f)
        c.steer = padAxis(PAD_StickX(i));
    c.accel = std::max(c.accel, padTrigger(PAD_TriggerR(i)));
    c.brake = std::max(c.brake, padTrigger(PAD_TriggerL(i)));
    c.shiftUp |= (p & (PAD_BUTTON_A | PAD_BUTTON_UP)) != 0;
    c.shiftDown |= (p & (PAD_BUTTON_B | PAD_BUTTON_DOWN)) != 0;
    c.view |= (p & PAD_BUTTON_X) != 0;
    c.start |= (p & PAD_BUTTON_START) != 0;
    c.coin |= (p & PAD_TRIGGER_Z) != 0;
    c.test |= (p & PAD_BUTTON_LEFT) != 0;
    c.service |= (p & PAD_BUTTON_RIGHT) != 0;
    c.quit |= (p & PAD_TRIGGER_Z) && (p & PAD_BUTTON_START) && (p & PAD_BUTTON_Y);

    // Wiimote (+ Nunchuk or Classic Controller).
    WPADData* w = WPAD_Data(i);
    if (!w || w->err != WPAD_ERR_NONE)
        return;
    const u32 b = WPAD_ButtonsHeld(i);
    c.start |= (b & (WPAD_BUTTON_PLUS | WPAD_CLASSIC_BUTTON_PLUS)) != 0;
    c.coin |= (b & (WPAD_BUTTON_MINUS | WPAD_CLASSIC_BUTTON_MINUS)) != 0;
    c.quit |= (b & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME)) != 0;
    if (w->exp.type == WPAD_EXP_NUNCHUK || w->exp.type == WPAD_EXP_CLASSIC)
    {
        // Test / service on the D-pad (a bare Wiimote shifts with it).
        c.test |= (b & (WPAD_BUTTON_LEFT | WPAD_CLASSIC_BUTTON_LEFT)) != 0;
        c.service |= (b & (WPAD_BUTTON_RIGHT | WPAD_CLASSIC_BUTTON_RIGHT)) != 0;
    }
    if (w->exp.type == WPAD_EXP_NUNCHUK)
    {
        // Nunchuk stick steers; A accelerates, B (trigger) brakes;
        // Z / C shift up / down; 1 changes the view.
        if (stickX(w->exp.nunchuk.js) != 0.f)
            c.steer = stickX(w->exp.nunchuk.js);
        if (b & WPAD_BUTTON_A)
            c.accel = 1.f;
        if (b & WPAD_BUTTON_B)
            c.brake = 1.f;
        c.shiftUp |= (b & WPAD_NUNCHUK_BUTTON_Z) != 0;
        c.shiftDown |= (b & WPAD_NUNCHUK_BUTTON_C) != 0;
        c.view |= (b & WPAD_BUTTON_1) != 0;
    }
    else if (w->exp.type == WPAD_EXP_CLASSIC)
    {
        // Classic: left stick steers, R / L (analog or ZR / ZL) are the
        // pedals, a / b shift up / down, x changes the view.
        const classic_ctrl_t& cc = w->exp.classic;
        if (stickX(cc.ljs) != 0.f)
            c.steer = stickX(cc.ljs);
        c.accel = std::max(c.accel, (b & WPAD_CLASSIC_BUTTON_ZR) ? 1.f : cc.r_shoulder);
        c.brake = std::max(c.brake, (b & WPAD_CLASSIC_BUTTON_ZL) ? 1.f : cc.l_shoulder);
        c.shiftUp |= (b & WPAD_CLASSIC_BUTTON_A) != 0;
        c.shiftDown |= (b & WPAD_CLASSIC_BUTTON_B) != 0;
        c.view |= (b & WPAD_CLASSIC_BUTTON_X) != 0;
    }
    else
    {
        // Wiimote alone, held sideways: 2 accelerates, 1 brakes,
        // the D-pad (rotated) steers and shifts.
        if (b & WPAD_BUTTON_UP)
            c.steer = -1.f;
        if (b & WPAD_BUTTON_DOWN)
            c.steer = 1.f;
        if (b & WPAD_BUTTON_2)
            c.accel = 1.f;
        if (b & WPAD_BUTTON_1)
            c.brake = 1.f;
        c.shiftUp |= (b & WPAD_BUTTON_RIGHT) != 0;
        c.shiftDown |= (b & WPAD_BUTTON_LEFT) != 0;
        c.view |= (b & WPAD_BUTTON_A) != 0;
    }
}

void readGameControls(GameControls& c)
{
    WPAD_ScanPads();
    PAD_ScanPads();
    c = GameControls();
    for (int i = 0; i < 4; i++)
        addPad(i, c);
    c.quit |= s_powerPressed || s_resetPressed;
}

void readGameControlsTwo(GameControls& p1, GameControls& p2)
{
    WPAD_ScanPads();
    PAD_ScanPads();
    p1 = GameControls();
    p2 = GameControls();
    for (int i = 0; i < 4; i++)
        addPad(i, i == 1 ? p2 : p1);
    p1.quit |= s_powerPressed || s_resetPressed || p2.quit;
}

std::vector<std::string> gameControlsHelp()
{
    return {
        "GameCube: stick steer, R/L gas/brake, A/B shift up/down, X view,",
        "          Start start, Z coin, D-pad left/right test/service,",
        "          Start+Z+Y quit",
        "Nunchuk : stick steer, A/B gas/brake, Z/C shift, 1 view, + start,",
        "          - coin, Home quit",
        "Classic : left stick steer, R/L gas/brake, a/b shift, x view,",
        "          + start, - coin, Home quit",
    };
}

const char* buttonLabel(uint32_t button)
{
    switch (button)
    {
    case BTN_A: return "A";
    case BTN_B: return "B";
    case BTN_1: return "1";
    case BTN_2: return "2";
    case BTN_EXIT: return "(-) + (+)";
    case BTN_LEFT | BTN_RIGHT: return "LEFT/RIGHT";
    case BTN_UP | BTN_DOWN: return "UP/DOWN";
    default: return "?";
    }
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

bool listDirectory(const std::string& path, std::vector<DirEntry>& out)
{
    out.clear();
    DIR* d = opendir(path.c_str());
    if (!d)
        return false;
    std::string base = path;
    if (!base.empty() && base.back() != '/')
        base += '/';
    while (struct dirent* e = readdir(d))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        DirEntry de;
        de.name = e->d_name;
        struct stat st;
        if (stat((base + de.name).c_str(), &st) == 0)
        {
            de.isDir = S_ISDIR(st.st_mode);
            de.size = (uint64_t)st.st_size;
        }
        out.push_back(de);
    }
    closedir(d);
    return true;
}

bool fileExists(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode);
}

uint64_t fileSize(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? (uint64_t)st.st_size : 0;
}

char pathSeparator()
{
    return '/';
}

int storageCount()
{
    return 2;
}

int currentStorage()
{
    return s_storage;
}

const char* storageLabel(int index)
{
    return index == 0 ? "SD CARD" : "USB";
}

bool selectStorage(int index)
{
    if (index == 0)
    {
        if (!s_sdMounted)
            s_sdMounted = dirExists("sd:/");
        if (!s_sdMounted)
            return false;
    }
    else
    {
        if (!mountUsb(3000))
            return false;
    }
    s_storage = index;
    return true;
}

std::string gamesRoot()
{
    const char* sd[] = {"sd:/initiald3/", "sd:/naomi/", "sd:/roms/", "sd:/"};
    const char* usb[] = {"usb:/initiald3/", "usb:/naomi/", "usb:/roms/", "usb:/"};
    const char** c = s_storage == 0 ? sd : usb;
    for (int i = 0; i < 4; i++)
        if (dirExists(c[i]))
            return c[i];
    return c[3];
}

std::string appDir()
{
    return s_appDir[0] ? std::string(s_appDir) : std::string(s_storage == 0 ? "sd:/" : "usb:/");
}

std::string logPath()
{
    return appDir() + "idas3wii_log.txt";
}

// ---------------------------------------------------------------------------
// Memory: carve from the TOP of the MEM2 arena, then of MEM1 when MEM2 is
// full. libogc's malloc grows from the bottom of each arena (MEM1 first, then
// spilling into MEM2), so taking from the top never moves memory the heap
// already owns; a later failed sbrk just makes malloc return NULL. A reserve
// is always left in both arenas for the C heap (libchdr's hunk map alone is
// ~1 MB for a GD-ROM).
// ---------------------------------------------------------------------------

static constexpr u32 kMem2Reserve = 2u << 20;
static constexpr u32 kMem1Reserve = 4u << 20;
static constexpr int kMaxBlocks = 8;

struct LargeBlock
{
    u8* base;
    u8* prevHi;
    bool mem1;
};
static LargeBlock s_blocks[kMaxBlocks];
static int s_blockCount = 0;

static s32 arenaFree(bool mem1)
{
    const u8* hi = (u8*)(mem1 ? SYS_GetArena1Hi() : SYS_GetArena2Hi());
    const u8* lo = (u8*)(mem1 ? SYS_GetArena1Lo() : SYS_GetArena2Lo());
    // Signed: an overrun arena must read as negative, not wrap into "plenty".
    return (s32)(hi - lo) - (s32)(mem1 ? kMem1Reserve : kMem2Reserve);
}

void* allocLarge(size_t bytes)
{
    if (s_blockCount >= kMaxBlocks)
        return nullptr;
    for (int pass = 0; pass < 2; pass++)
    {
        const bool mem1 = pass == 1;
        u32 level = IRQ_Disable();
        if (arenaFree(mem1) < (s32)(bytes + 64))
        {
            IRQ_Restore(level);
            continue;
        }
        u8* hi = (u8*)(mem1 ? SYS_GetArena1Hi() : SYS_GetArena2Hi());
        u8* p = (u8*)((u32)(hi - bytes) & ~63u);
        if (mem1)
            SYS_SetArena1Hi(p);
        else
            SYS_SetArena2Hi(p);
        IRQ_Restore(level);
        s_blocks[s_blockCount++] = {p, hi, mem1};
        return p;
    }
    return nullptr;
}

void freeLarge(void* p, size_t)
{
    // Only the most recent block can be returned to its arena.
    if (s_blockCount == 0 || s_blocks[s_blockCount - 1].base != p)
        return;
    const LargeBlock& b = s_blocks[--s_blockCount];
    u32 level = IRQ_Disable();
    if (b.mem1)
        SYS_SetArena1Hi(b.prevHi);
    else
        SYS_SetArena2Hi(b.prevHi);
    IRQ_Restore(level);
}

size_t largeFreeBytes()
{
    const s32 m1 = arenaFree(true), m2 = arenaFree(false);
    const s32 best = m1 > m2 ? m1 : m2;
    return best > 0 ? (size_t)best : 0;
}

std::string memoryReport()
{
    char buf[96];
    snprintf(buf, sizeof(buf), "MEM1 %.1f MB free, MEM2 %.1f MB free",
             (s32)((u8*)SYS_GetArena1Hi() - (u8*)SYS_GetArena1Lo()) / (1024.0f * 1024.0f),
             (s32)((u8*)SYS_GetArena2Hi() - (u8*)SYS_GetArena2Lo()) / (1024.0f * 1024.0f));
    return buf;
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

uint64_t microseconds()
{
    return ticks_to_microsecs(gettime());
}

void sleepMs(int ms)
{
    usleep((useconds_t)ms * 1000);
}

} // namespace platform
} // namespace idas3

#endif // GEKKO
