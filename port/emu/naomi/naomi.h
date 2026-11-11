// NAOMI 2 platform layer for the forked nullDC4Wii core (Initial D3 port).
//
// The Dreamcast core is reused as-is; this folder supplies what a NAOMI 2
// has instead of a Dreamcast's GD-ROM drive and BIOS:
//   naomi_board.cpp  cartridge/DIMM-board registers at 0x005F7000 and the
//                    G1 cart DMA (it implements dc/gdrom/gdrom_if.h, so the
//                    core's calls into "the GD-ROM" reach the NAOMI board)
//   naomi_boot.cpp   BIOS-free boot: load the program like the BIOS does and
//                    start the game at its entry point
//   naomi_jvs.cpp    maple port A: the main board's MIE (maple/JVS bridge,
//                    board DIP switches, TEST/SERVICE, EEPROM) and the JVS
//                    I/O board the controls are wired to
//   naomi_trace.cpp  bring-up diagnostics (unhandled accesses, PC samples)
#pragma once

#include "types.h"

// ---- Game image (decrypted DIMM contents) --------------------------------
// The port's DimmImage provides on-demand decrypted reads; the core never
// sees the CHD.
typedef bool (*NaomiCartReadFn)(void* user, u32 offset, u32 length, u8* dst);
void naomi_SetCart(NaomiCartReadFn read, void* user, u32 size);
bool naomi_CartRead(u32 offset, u32 length, u8* dst);
u32 naomi_CartSize();
// Host work on the cart image running in the background: while `busy` says so,
// each vblank gives it a slice of the frame (naomi_CartSlice).
typedef bool (*NaomiCartBusyFn)(void* user);
void naomi_SetCartBusy(NaomiCartBusyFn busy);
void naomi_CartSlice();

// ---- Diagnostics -----------------------------------------------------------
struct NaomiStats
{
    u32 cartDmaCount;
    u32 cartDmaBytes;
    u32 cartDmaPolls;   // SB_GDST reads while a transfer ran
    u32 cartPioReads;
    u32 dimmCommands;
    u32 boardRegReads;
    u32 boardRegWrites;
};
const NaomiStats& naomi_Stats();
void naomi_BoardLogCounters();      // bring-up: accesses per board register

// printf-style, routed to the port's run log (set by the host).
typedef void (*NaomiLogFn)(const char* text);
void naomi_SetLog(NaomiLogFn fn);
void naomi_Log(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// ---- Frame heartbeat --------------------------------------------------------
// The core calls naomi_VBlank() at every emulated vblank and
// naomi_OnStartRender() at every STARTRENDER. A host hook set with
// naomi_SetFrameHook() runs every `period` vblanks with the running totals
// (used by the Wii build to log progress while the screen is still black).
struct NaomiFrameInfo
{
    u32 vblanks;
    u32 renders;
    u32 pc;            // SH-4 PC (block granularity under the recompiler)
};
typedef void (*NaomiFrameHook)(const NaomiFrameInfo& info);
void naomi_SetFrameHook(NaomiFrameHook fn, u32 period);
// REFLECTIONS: where a skipped reflection build leaves from pc (0: not its
// block, or every build is made), and whether the build at that row is skipped.
extern "C" u32 naomi_ReflectionExit(u32 pc);
extern "C" u32 naomi_ReflectionSkip(u32 row);
// MIRROR = OFF + GAME SKIP: nonzero if the block at pc is the game's per-view
// draw (0C024100); the CPU back ends then call naomi_MirrorViewSkip(r4) at its
// entry and return to the caller (pc = pr) if it says so (the mirror's view).
extern "C" u32 naomi_MirrorViewEntry(u32 pc);
extern "C" u32 naomi_MirrorViewSkip(u32 view);
extern "C" u32 g_mirror_pass_skips;
// 2 PLAYERS (naomi_2p.cpp): split-screen races, the second pad driving the
// CPU car. The host passes the second pad each poll; the core calls the two
// hooks from naomi_VBlank() and naomi_OnStartRender().
struct Naomi2pPad
{
    float steer;      // -1 full left .. +1 full right
    float accel;      // 0 .. 1
    float brake;      // 0 .. 1
    bool shiftUp, shiftDown;
};
extern "C" int g_two_player_preset;
extern "C" int g_p2_car_preset;       // 0 ON RAILS, 1 FULL PHYSICS
extern "C" int g_p2_scenery_preset;   // 0 AROUND P1, 1 AROUND EACH CAR
void naomi_2pSetPad(const Naomi2pPad& pad);
const Naomi2pPad& naomi_2pPad();
void naomi_2pVBlank();
void naomi_2pStartRender();
// The HUD speed of the second player's view: nonzero if the block at pc is
// where the HUD reads the meter's speed (the CPU back ends then call
// naomi_2pHud with the guest's r14 at that block's entry).
extern "C" u32 naomi_2pHudEntry(u32 pc);
extern "C" void naomi_2pHud(u32 r14);
// P2 CAR = FULL PHYSICS: nonzero (1, 2) if the block at pc is one of the two
// returns in the drivers update where the player's physics is run again for
// player 2; the CPU back ends then call naomi_2pPhys(that) at the block's
// entry and continue at the address it returns, if not 0.
extern "C" u32 naomi_2pPhysEntry(u32 pc);
extern "C" u32 naomi_2pPhys(u32 which);
// P2 SCENERY: nonzero if the block at pc is the course's update (the CPU back
// ends then call naomi_2pCourse with the guest's r4 at its entry).
extern "C" u32 naomi_2pCourseEntry(u32 pc);
extern "C" void naomi_2pCourse(u32 r4);
// ... and 1 or 2 if it is the view update's setSegment call or its return
// (naomi_2pView(that, the guest's r12) at the block's entry).
extern "C" u32 naomi_2pViewEntry(u32 pc);
extern "C" void naomi_2pView(u32 which, u32 r12);
// MOTION FX (naomi_motion.cpp): how the camera of a race view moves, read at
// every vblank (naomi_MotionVBlank) for the renderer's effects. half: the 2
// PLAYERS half of the render (-1 whole picture, 1 the CPU car's view).
struct NaomiMotion
{
    bool valid;       // a race view following a car
    float step;       // metres the car moves a frame
    float kmh;
    float yawStep;    // radians a frame, + turning to the driver's left
    float focal;      // the scene's projection: screen pixels (640 wide) a unit of X / Z
    bool inCar;       // the in-car view (no part of the player's car on screen)
};
extern "C" int g_motion_fx_preset;      // 0 OFF, 1 SPEED BLUR, 2 FRAME BLEND, 3 IN-BETWEEN, 4 BLUR + IN-BETWEEN
// IN-BETWEEN's depth mask (gxRend.cpp): a byte a pixel of the largest EFB.
// _vmem.cpp leaves the heap this much more for it.
constexpr u32 kMotionMaskBytes = 640u * 576u;
inline bool naomi_MotionInBetween() { return g_motion_fx_preset == 3 || g_motion_fx_preset == 4; }
extern "C" int g_motion_level_preset;   // 1 LOW, 2 MEDIUM, 3 HIGH
extern "C" int g_between_warp_preset;   // IN-BETWEEN picture: 0 SMOOTH (one pass), 1 BANDS (three depth bands), 2 ROTATION ONLY, 3 NO MOVE
extern "C" bool g_motion_trace_now;     // runner: IDAS3_MOTION's frames
extern "C" bool naomi_Motion(int half, NaomiMotion* m);
extern "C" void naomi_MotionProjection(float fx, float tx, float fy, float ty);
extern "C" void naomi_MotionList(u32 vertices);
void naomi_MotionVBlank();
extern "C" bool naomi_RaceView();
// SOFT FLOAT (naomi_softfloat.cpp): the native version of the game's software
// double divide or multiply starting at pc (null: not one, or the option is
// off). Called with r4-r7, it returns 0 to leave the call to the game's code,
// else 1 with the result (r0, r1) in g_soft_float_result.
typedef u32 (*NaomiSoftFloatFn)(u32 r4, u32 r5, u32 r6, u32 r7);
extern "C" NaomiSoftFloatFn naomi_SoftFloatEntry(u32 pc);
extern "C" u32 naomi_SoftFloatDiv(u32 r4, u32 r5, u32 r6, u32 r7);
extern "C" u32 naomi_SoftFloatMul(u32 r4, u32 r5, u32 r6, u32 r7);
extern "C" u32 g_soft_float_result[2];
extern "C" int g_soft_float_preset;
void naomi_VBlank();
void naomi_OnStartRender();
// Called from the update cascade: logs the CPU state once if execution ever
// reaches BIOS ROM (the game never runs the BIOS; getting there means a
// crash, a reset or a boot that did not take).
void naomi_CheckPc();
// Precise version, called by the CPU back ends at the moment execution
// reaches BIOS ROM (`from` = the jumping instruction, 0 if unknown): logs
// the CPU state, the stack and recently compiled blocks, once.
void naomi_ReportBiosEntry(u32 pc, u32 from);
// The recompiler records every block it compiles (shown by the report).
void naomi_NoteBlock(u32 pc);
// Called when the game's own assert handler fires (it paints the border and
// spins forever): logs which check failed and the CPU state, once.
void naomi_ReportAssert();

// ---- Optional BIOS ROM ------------------------------------------------------
// The boot itself never runs BIOS code, but the game copies the BIOS's
// runtime service library (28 KB at ROM offset 0x60000) to 0x0C018000 and
// calls it through a table. When an image is set it is placed in the BIOS
// ROM area at boot so that copy succeeds. The data must stay valid while the
// core runs. NAOMI 1 and NAOMI 2 BIOS images both carry the library.
void naomi_SetBiosImage(const u8* data, u32 size);

// ---- Controls (naomi_jvs.cpp) ----------------------------------------------
// Set by the host every frame. Player bits follow the JVS switch layout,
// high byte first: START 0x8000, SERVICE 0x4000, UP 0x2000, DOWN 0x1000,
// LEFT 0x0800, RIGHT 0x0400, BUTTON1..8 0x0200 down to 0x0004.
// Initial D: VIEW = P1 DOWN, shifter up/down = P2 UP/DOWN.
// Analog channels are 16-bit, 0x8000 = centre: Initial D uses 0 steering,
// 1 accelerator, 2 brake (pedals 0 = released).
enum : u32
{
    NAOMI_JVS_START = 0x8000,
    NAOMI_JVS_SERVICE = 0x4000,
    NAOMI_JVS_UP = 0x2000,
    NAOMI_JVS_DOWN = 0x1000,
    NAOMI_JVS_LEFT = 0x0800,
    NAOMI_JVS_RIGHT = 0x0400,
    NAOMI_JVS_BUTTON1 = 0x0200,
    NAOMI_JVS_BUTTON2 = 0x0100,
};
struct NaomiInputs
{
    bool test;          // JVS system TEST switch (operator menu)
    bool boardTest;     // main board TEST button (read through the MIE)
    bool boardService;  // main board SERVICE button
    bool coin[2];       // coin chutes, counted on press
    u16 players[2];
    u16 analog[8];
};
void naomi_SetInputs(const NaomiInputs& in);
void naomi_DefaultInputs(NaomiInputs& in);   // nothing pressed, centred
// Optional: called by the core whenever the game polls its controls (once or
// twice per frame), so the host reads its pads at the right moment. It may
// also call naomi_Stop() to leave the game.
typedef void (*NaomiInputPollFn)(NaomiInputs& in);
void naomi_SetInputPoll(NaomiInputPollFn fn);

// The 128-byte serial EEPROM on the main board (system/game settings the
// BIOS and the game keep). Optional persistence: the host loads it before
// starting and can save naomi_Eeprom() when naomi_EepromDirty() says so.
void naomi_SetEeprom(const u8* data128);
const u8* naomi_Eeprom();
bool naomi_EepromDirty(bool clear);

struct NaomiJvsStats
{
    u32 mapleFrames;     // frames addressed to the MIE
    u32 jvsMessages;     // JVS packets delivered to the I/O board
    u32 inputReads;      // 0x20.. input requests
    u32 unknown;         // unsupported maple commands / sub-commands
};
const NaomiJvsStats& naomi_JvsStats();

// ---- Card reader/writer (naomi_card.cpp) -----------------------------------
// Initial D's Sanwa magnetic card unit on the SH-4 serial port. Starts with
// no card (the game then plays without saving progress).
void naomi_CardReaderAttach();
void naomi_CardInsert(bool inserted);
void naomi_CardSetData(const u8* data, u32 size);            // 3 x 0x45 bytes
const u8* naomi_CardData(u32* size, bool* dirty);             // dirty: written since last call
u32 naomi_CardCommands();

// ---- BIOS-free boot (naomi_boot.cpp) -------------------------------------
// Performed from dc_elf_pre_run_hook(), i.e. right after the CPU reset.
// Returns false (and logs why) when the image has no valid NAOMI header.
bool naomi_BiosFreeBoot();
