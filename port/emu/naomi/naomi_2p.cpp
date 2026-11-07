// 2 PLAYERS: split-screen races for two (launcher GENERAL page).
//
// Initial D Arcade Stage 3 is a one-player game: a race pits the player's car
// against a CPU car. With 2 PLAYERS on, the picture is split side by side and
// the second pad drives the CPU car. The game still draws one view a frame:
// the cars take turns being the one its camera follows, and the renderer
// squeezes each view into its car's half of the picture, keeping the other
// half (gxRend split2p_*, soft_renderer splitPut). Each player sees 30 views a
// second; the Wii draws no more than in a one-player race.
//
// What the game is told, found on the runner (GDS-0033; every pointer is
// checked against the object's class before use, so other code is untouched):
// - [0C8FF174] is the race camera. It takes its world matrix from the object it
//   is attached to ([camera+390], class 0C386EFC), which hangs off a car-follow
//   rig ([object+4], class 0C386CDC, the in-car and the chase view alike). The
//   rig follows the car block at [rig+F8] through that car's world matrix at
//   [rig+FC] (block + 964). Pointing both at the CPU car's block puts the game's
//   own in-car camera in the CPU car: the next frame is drawn from there.
// - A car block's object (+93C) holds the car's state record at +20 (block +
//   95C): the CPU car's is the static record 0C8FF430. The two blocks are
//   0x3200 apart.
// - The camera builds its projection every frame from its aspect at
//   [camera+B8] (4:3); half the picture's aspect gives each half undistorted
//   views (narrower to the sides, the game's frustum culling stays wide).
// - A game frame starts with STARTRENDER, right after the vblank, of the scene
//   built in the previous frame; then it updates the HUD meter and builds the
//   next scene, and only then updates the rig (0C09D7C0) and the camera matrix.
//   So a render shows the car the rig followed two STARTRENDERs before it
//   (runner timeline, IDAS3_TIMELINE). The cars change at every STARTRENDER,
//   never held back: holding one for a dropped frame (the first version) made
//   the next two views land in the wrong halves on a Wii (owner: flicker).
// - The HUD meter (class 0C3873C4, speed in km/h at +54, RPM at +58) is
//   updated from the player's car at 0C06589C, then read where the HUD is
//   drawn (block 0C0C9A98, meter = [r14+4]: revs at 0C0C9AA8, speed at
//   0C0C9BD0): the HUD of the CPU car's view is given that car's values there
//   (naomi_2pHud). That HUD is shown two renders later, the 3D scene built in
//   the same frame one.
// - The CPU car is not the player's physics: its motion body (0C901F38, code
//   0C15B000-0C15CFFF) moves along its heading (+AC, radians) at its speed
//   (+44, km/h) every frame. Its AI aims at a point of its racing line ahead
//   (the heading error feeds a damped yaw rate) and changes the speed by a
//   bounded step, holding it at 0 until the race runs (bit 15 of
//   [[0C900954]+50]). Player 2 sets the speed and nudges the heading at every
//   vblank. The AI's pull toward its line balances the nudge: holding the
//   stick moves the car across the road, letting go brings it back. The car
//   stays on the course (a free heading drives through the guardrails).
// - P2 CAR = FULL PHYSICS (owner, 2026-11-07: "on rails" was not driving).
//   The player's physics is one routine run by the drivers update 0C159920
//   (fr4 = dt -> 0C901650): 0C157880 (both cars' collision probes 0C401B04 /
//   0C401D74, 0x270 each, built from the render states 0C8FF388 / 0C8FF430,
//   0xA8 each; car-car collision 0C0C06C0), then 0C157AE0 (record =
//   [0C900954] = 0C9008A4; simulation 0C15CEC0 on the core state 0C900F00,
//   which reads the controls as the JVS analog words at 0C92F0E0; 0C157A80
//   copies the record's first 160 bytes to 0C8FF388, the AI's 0C9017D4 to
//   0C8FF430), then the CPU car's AI 0C15B0A0(1, 1). The code names ~50 record
//   fields by address, so player 2 gets the whole state block (0C9008A4-
//   0C9017D3) swapped in and the two calls run again (naomi_2pPhys), plus its
//   own copy of what the physics writes elsewhere (kOutside: runner census,
//   IDAS3_PHYSW), the race's fields of the block taken from player 1's
//   (p2Prepare), and the drivers update's wheels-on-the-road save and restore
//   done for it (p2AfterRun). The CPU car is then put where player 2's
//   physics left it (its render state and the AI body: position in the race,
//   sounds). Player 2's car starts as a copy of player 1's moved to the CPU
//   car's place on the grid (initP2). Not done: player 2 shifting by hand (the
//   gear is in the block, 0C900E88; where the shift requests come from is not
//   known), so with a manual gearbox chosen player 2 stays in its gear.
// - P2 SCENERY = AROUND EACH CAR (owner: from ~800 m apart player 2's view
//   kept only the far backdrop; runner: fine at 113 m, gone at 922 m). The
//   course is 9 areas and draws 3 (the comment above findView); the area
//   comes from the road segment of the view's own car. How it was found: the
//   course model calls are written into the frame's ELAN list by 0C1D8060
//   (IDAS3_WRITEVAL with a stack backtrace on a model player 2's view lacked)
//   from the course renderer 0C19EEC0. Ruled out on the way: the camera (held
//   on player 2 for 190 frames, no change), swapping the view's car (+418:
//   ADVANTAGE breaks, the segment tracker steps from player 1's segment).
#include "types.h"
#include "dc/mem/sh4_mem.h"
#include "naomi.h"
#include "naomi_elan.h"

#include <cmath>
#include <cstring>

extern "C" {
int g_two_player_preset = 0;          // 0 OFF, 1 SIDE BY SIDE
int g_p2_car_preset = 1;              // 0 ON RAILS (the AI body), 1 FULL PHYSICS
int g_p2_scenery_preset = 1;          // 0 AROUND P1 (the game's), 1 AROUND EACH CAR
int g_2p_hold = -1;                   // runner: >= 0 keeps the camera on that car (0 player, 1 CPU)
float g_split2p_picture_aspect = 4.f / 3.f;   // the whole picture's, set by the renderer
}

namespace {

const u32 kCameraPtr = 0x0C8FF174;    // -> the race camera
const u32 kCamAttach = 0x390;         // camera -> object it is attached to
const u32 kCamAspect = 0xB8;          // camera's aspect (projection, every frame)
const u32 kAttachClass = 0x0C386EFC;
const u32 kRigClass = 0x0C386CDC;
const u32 kRigCar = 0xF8;             // rig -> car block it follows
const u32 kRigCarMatrix = 0xFC;       // rig -> that car's world matrix
const u32 kCarMatrix = 0x964;         // car block -> world matrix
const u32 kCarState = 0x95C;          // car block -> pointer to its state record
const u32 kCpuState = 0x0C8FF430;     // the CPU car's state record
const u32 kCarBlockStride = 0x3200;
const u32 kAspect43 = 0x3FAAAAAB;     // 4/3, the game's
const u32 kCpuBody = 0x0C901F38;      // the CPU car's motion body
const u32 kBodySpeed = 0x44;          // km/h
const u32 kBodyHeading = 0xAC;        // radians
const u32 kBodyYawRate = 0xB0;
const u32 kBodyPos = 0xC8;            // x y z, as the state record's
const u32 kRaceInfo = 0x0C900954;     // -> the race's flags (+50, bit 15: running)
const u32 kHudBlock = 0x0C0C9A98;     // HUD draw, meter = [r14+4]: reads the revs (+58) at 0C0C9AA8, the speed (+54) at 0C0C9BD0
const u32 kMeterClass = 0x0C3873C4;
const u32 kMeterSpeed = 0x54;         // km/h, what the HUD shows (truncated)
const u32 kMeterRevs = 0x58;          // rpm, the rev counter's needle

// Player 2's car, tuned on the runner: gas and brake in km/h a frame, the
// heading nudge in radians a frame at full lock (the AI's pull balances it a
// few metres off its line). The heading grows to the driver's left.
const float kGasRate = 0.45f;         // ~0-100 km/h in 4 s
const float kBrakeRate = 1.6f;
const float kCoastRate = 0.06f;
const float kTopSpeed = 170.f;
const float kSteerRate = 0.04f;

Naomi2pPad s_pad;          // the second pad, from the host
bool s_active = false;     // a race is on and drawn split
// The car (0 player, 1 CPU) the rig was given at the last STARTRENDER [0] and
// the one before [1]: the scene being built is [1]'s, the next render too.
int s_target[2];
int s_renderHalf = -1;     // half of the render in flight, -1 whole picture
int s_mirrorSaved = 1;     // the MIRROR option, while split screen turns it off
float s_speed2 = 0.f;      // player 2's speed (km/h), kept here: the AI's own step never adds up
bool s_driving2 = false;   // the race runs and player 2 has the CPU car
u32 s_frames = 0;          // vblanks with split screen on, for the [2P] log line
u32 s_drawn[2];            // views drawn in each half since that line

struct Race
{
    u32 camera, rig, player, cpu;
};
Race s_race;

bool inRam(u32 a)
{
    return (a & 0x1E000003) == 0x0C000000;
}

// The race camera, its rig and the two cars, or false outside a race (or in a
// view that is not attached to a car-follow rig: replays, fly-bys).
bool findRace(Race& race)
{
    const u32 camera = ReadMem32(kCameraPtr);
    if (!inRam(camera))
        return false;
    const u32 attach = ReadMem32(camera + kCamAttach);
    if (!inRam(attach) || ReadMem32(attach) != kAttachClass)
        return false;
    const u32 rig = ReadMem32(attach + 4);
    if (!inRam(rig) || ReadMem32(rig) != kRigClass)
        return false;
    // The rig follows the player's car unless this module pointed it at the
    // CPU car: tell the two blocks apart by their state records.
    const u32 car = ReadMem32(rig + kRigCar);
    if (!inRam(car))
        return false;
    u32 player, cpu;
    if (ReadMem32(car + kCarState) == kCpuState)
    {
        cpu = car;
        player = car + kCarBlockStride;
        if (ReadMem32(player + kCarState) == kCpuState || !inRam(ReadMem32(player + kCarState)))
            player = car - kCarBlockStride;
    }
    else
    {
        player = car;
        cpu = car - kCarBlockStride;
        if (ReadMem32(cpu + kCarState) != kCpuState)
            cpu = car + kCarBlockStride;
    }
    if (ReadMem32(cpu + kCarState) != kCpuState || !inRam(ReadMem32(player + kCarState)) ||
        ReadMem32(player + kCarState) == kCpuState)
        return false;
    race = {camera, rig, player, cpu};
    return true;
}

void followCar(const Race& race, int car)
{
    const u32 block = car ? race.cpu : race.player;
    WriteMem32(race.rig + kRigCar, block);
    WriteMem32(race.rig + kRigCarMatrix, block + kCarMatrix);
}

// P2 SCENERY = AROUND EACH CAR. The course (class 0C38DE4C, from the
// player's view object: camera - 80 (class 0C3831C4), +40C) draws 3 of its 9
// areas, around the current one (+48, renderer 0C19EEC0), which its
// setSegment (vtable +28, 0C03A2E0) takes from the road segment the view
// passes it: the view's update (0C05FE48-0C05FE66) hands it its own car's
// (view +584 + 12 x [+580], and +5B4). The view tracks the opponent's segment
// too, the same way (+59C + 12 x [+580], +5C0: ADVANTAGE), wherever it is.
// For a scene of player 2's view the two are swapped around that one call
// (naomi_2pView), and the course's update (0C19B060) follows the CPU car's
// matrix (+50) instead of the player's (naomi_2pCourse). Like the HUD, what
// the view sets up in a frame is drawn two renders later: the swap is for the
// car the rig was just given ([0]; with [1] player 1's view lost its course).
const u32 kViewFromCamera = 0x80;
const u32 kViewClass = 0x0C3831C4;    // at view +C
const u32 kViewCourse = 0x40C;
const u32 kViewSlot = 0x580;          // which of the two segment slots is this frame's
const u32 kViewOwnSeg = 0x584, kViewOwnSeg2 = 0x5B4;   // the view's car: 12-byte slots, and a word
const u32 kViewOppSeg = 0x59C, kViewOppSeg2 = 0x5C0;   // the opponent's
const u32 kViewSetSegment = 0x0C05FE48; // return from the course update: loads and calls setSegment
const u32 kViewAfterSet = 0x0C05FE6A;   // return from setSegment
const u32 kCourseClass = 0x0C38DE4C;
const u32 kCourseUpdate = 0x0C19B060;
const u32 kCourseTarget = 0x50;       // -> the followed car's matrix

u32 s_viewSwapped = 0;                // the view whose segments are swapped now, 0: none
u32 s_courseSwaps = 0;                // for the [2P] line

u32 findView(const Race& race)
{
    const u32 view = race.camera - kViewFromCamera;
    return inRam(view) && ReadMem32(view + 0xC) == kViewClass ? view : 0;
}

void swapWords(u32 a, u32 b)
{
    const u32 x = ReadMem32(a);
    WriteMem32(a, ReadMem32(b));
    WriteMem32(b, x);
}

// The view's own and opponent segments exchanged (twice puts them back).
void swapViewSegments(u32 view)
{
    const u32 slot = 12 * (ReadMem32(view + kViewSlot) & 1);
    swapWords(view + kViewOwnSeg + slot, view + kViewOppSeg + slot);
    swapWords(view + kViewOwnSeg2, view + kViewOppSeg2);
}

float readFloat(u32 a)
{
    const u32 bits = ReadMem32(a);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

void writeFloat(u32 a, float f)
{
    u32 bits;
    memcpy(&bits, &f, 4);
    WriteMem32(a, bits);
}

// Player 2 drives the CPU car (see the top of the file). Until the race runs
// the AI keeps it still; player 2 takes over from the AI's speed at the start.
void driveCpuCar(const Naomi2pPad& pad)
{
    // The body must be the CPU car's: same position as its state record.
    const float bx = readFloat(kCpuBody + kBodyPos), bz = readFloat(kCpuBody + kBodyPos + 8);
    const float sx = readFloat(kCpuState), sz = readFloat(kCpuState + 8);
    if (!(bx - sx < 2.f && sx - bx < 2.f && bz - sz < 2.f && sz - bz < 2.f))
    {
        s_driving2 = false;
        return;
    }
    const u32 info = ReadMem32(kRaceInfo);
    const bool running = inRam(info) && (ReadMem32(info + 0x50) >> 15 & 1);
    if (!running)
    {
        s_driving2 = false;
        return;
    }
    if (!s_driving2)
    {
        s_speed2 = readFloat(kCpuBody + kBodySpeed);
        s_driving2 = true;
    }
    // What the frame ended with: player 2's speed plus the AI's step. The AI
    // brakes when the car is too fast for the bend ahead: then player 2 cannot
    // take it faster either (the CPU car never slides off).
    const float seen = readFloat(kCpuBody + kBodySpeed);
    float v = s_speed2;
    v += pad.accel * kGasRate - pad.brake * kBrakeRate;
    if (pad.accel < 0.05f && pad.brake < 0.05f)
        v -= kCoastRate;
    if (seen < s_speed2 && v > seen)
        v = seen;
    v = v < 0.f ? 0.f : v > kTopSpeed ? kTopSpeed : v;
    s_speed2 = v;
    writeFloat(kCpuBody + kBodySpeed, v);
    // Turning needs speed: no nudge standing still, full from ~40 km/h.
    const float grip = v > 40.f ? 1.f : v / 40.f;
    if (pad.steer != 0.f)
        writeFloat(kCpuBody + kBodyHeading, readFloat(kCpuBody + kBodyHeading) - pad.steer * kSteerRate * grip);
}

void setAspect(const Race& race, bool split)
{
    float aspect = g_split2p_picture_aspect * 0.5f;
    u32 bits;
    memcpy(&bits, &aspect, 4);
    WriteMem32(race.camera + kCamAspect, split ? bits : kAspect43);
}

// ---- FULL PHYSICS (P2 CAR) --------------------------------------------------
// The player's physics runs a second time a frame for player 2, on player 2's
// copy of the player's state block (the top of the file). The drivers update
// at 0C159920 calls 0C157880 then 0C157AE0 (the player), then the CPU car's AI
// 0C15B0A0(1, 1), then 0C157A80 (render states). At the return from the
// player's physics the block is swapped and the two calls run again from
// 0C159938; at the second return it is swapped back. After the AI, the CPU
// car is put where player 2's physics left it.
const u32 kRerun = 0x0C159938;         // r1 = 0C157880; jsr; r14 = r15; then 0C157AE0
const u32 kAfterPhysics = 0x0C159944;  // return from 0C157AE0: r1 = AI; r4 = r5 = 1; jsr
const u32 kAfterAi = 0x0C15994C;       // return from the AI: r1 = 0C157A80; jsr
const u32 kBlockLo = 0x0C9008A4;       // the player's state (record, physics core, globals)
const u32 kBlockHi = 0x0C9017D4;
const u32 kBlockWords = (kBlockHi - kBlockLo) / 4;
const u32 kRenderP1 = 0x0C8FF388;      // render states, the record's first 160 bytes
const u32 kRenderCpu = 0x0C8FF430;
const u32 kRenderCpuCopy = 0x0C9017D4; // the AI's, copied to kRenderCpu by 0C157A80
const u32 kRenderWords = 40;           // the game's copies (0C222372) count 8-byte units: 20
const u32 kRenderCarWords = 9;         // position, velocity a frame, pitch yaw roll: the CPU car's too
const u32 kProbes = 0x0C401B04;        // both cars' collision probes, 0x270 each
const u32 kProbeWords = 2 * 0x270 / 4;
const u32 kSlotWords = 0xA8 / 4;       // a render state's slot (the CPU car's follows at +A8)

// The car's state the physics keeps outside the block (runner, IDAS3_PHYSW:
// every word it writes): the vehicle (wheels, controls, gearbox, engine:
// 0CAA94B4-0CAA9CEF), the drivetrain (0C91FB0C), the engine sound's state
// (0C99A904, 0C99AA94), what the engine and tyre sounds are made from (gas,
// gear, revs: 0C2FB3A4, 0C2FBB1C, 0CA9B4E8) and three records it writes now
// and then. Player 2 has its own copy of each, so player 1 keeps hearing its
// own car. Left shared: the matrix stack and scratch, the random numbers, the
// skid mark and wheel-motor queues.
struct Range
{
    u32 lo, words;
};
const Range kOutside[] = {{0x0CAA94B4, 0x83C / 4}, {0x0C91FB0C, 17}, {0x0C99A904, 21}, {0x0C99AA94, 22},
                          {0x0C2FB3A4, 12},         {0x0C2FBB1C, 2},  {0x0CA9B4E8, 9},  {0x0C9032CC, 5},
                          {0x0C92DCA0, 5},          {0x0C92DFC4, 15}};
const u32 kOutsideWords = 0x83C / 4 + 17 + 21 + 22 + 12 + 2 + 9 + 5 + 5 + 15;
const u32 kVehicleSpeed = 0x3C0 / 4;   // in the vehicle (first range): m/s
const u32 kVehicleRevs = 0x3F8 / 4;    // rpm, as the meter's

u32 s_p1[kBlockWords], s_p2[kBlockWords];
u32 s_p1Out[kOutsideWords], s_p2Out[kOutsideWords];
u32 s_p2SavedRender[kRenderWords];   // player 2's last render state with four wheels on the road
u32 s_probes[kProbeWords];
u32 s_slots[2 * kSlotWords];         // both render states before player 2's run

void readOutside(u32* w)
{
    for (const Range& rg : kOutside)
        for (u32 i = 0; i < rg.words; i++)
            *w++ = ReadMem32(rg.lo + 4 * i);
}

void writeOutside(const u32* w)
{
    for (const Range& rg : kOutside)
        for (u32 i = 0; i < rg.words; i++)
            WriteMem32(rg.lo + 4 * i, *w++);
}
bool s_p2Ready = false;    // s_p2 holds player 2's car
bool s_raceRunning = false;
bool s_inP2Run = false;    // the physics running now is player 2's
u32 s_p2Runs = 0;          // for the [2P] line

void readWords(u32 a, u32* w, u32 n)
{
    for (u32 i = 0; i < n; i++)
        w[i] = ReadMem32(a + 4 * i);
}

void writeWords(u32 a, const u32* w, u32 n)
{
    for (u32 i = 0; i < n; i++)
        WriteMem32(a + 4 * i, w[i]);
}

float wordFloat(u32 w)
{
    float f;
    memcpy(&f, &w, 4);
    return f;
}

u32 floatWord(float f)
{
    u32 w;
    memcpy(&w, &f, 4);
    return w;
}

bool physicsCode()
{
    // The drivers update as the hooks expect it (opcodes at the call sites).
    return ReadMem16(kRerun) == 0xD12B && ReadMem16(kRerun + 2) == 0x410B && ReadMem16(kAfterPhysics) == 0xD12A &&
           ReadMem16(kAfterPhysics + 2) == 0xE401 && ReadMem16(kAfterAi) == 0xD129;
}

// Player 2's car from player 1's, moved to where the CPU car stands: every
// position in the block (an x y z triple within a few metres of player 1's
// position) is moved by the gap between the two cars. Done on the starting
// grid, where both cars stand still and face the same way.
u32 movePositions(u32* w, u32 n, float x1, float y1, float z1, float dx, float dy, float dz)
{
    u32 moved = 0;
    for (u32 i = 0; i + 2 < n; i++)
    {
        const float x = wordFloat(w[i]), y = wordFloat(w[i + 1]), z = wordFloat(w[i + 2]);
        if (fabsf(x - x1) < 6.f && fabsf(y - y1) < 6.f && fabsf(z - z1) < 6.f)
        {
            w[i] = floatWord(x + dx);
            w[i + 1] = floatWord(y + dy);
            w[i + 2] = floatWord(z + dz);
            moved++;
            i += 2;
        }
    }
    return moved;
}

void initP2()
{
    readWords(kBlockLo, s_p2, kBlockWords);
    readOutside(s_p2Out);
    const float x1 = wordFloat(s_p2[0]), y1 = wordFloat(s_p2[1]), z1 = wordFloat(s_p2[2]);
    const float dx = readFloat(kRenderCpu) - x1, dy = readFloat(kRenderCpu + 4) - y1,
                dz = readFloat(kRenderCpu + 8) - z1;
    const u32 moved = movePositions(s_p2, kBlockWords, x1, y1, z1, dx, dy, dz) +
                      movePositions(s_p2Out, kOutsideWords, x1, y1, z1, dx, dy, dz);
    memcpy(s_p2SavedRender, s_p2, sizeof(s_p2SavedRender));
    s_p2Ready = true;
    naomi_Log("[2P] player 2's physics starts from player 1's, moved %.1f m (%u positions)\n",
              sqrtf(dx * dx + dy * dy + dz * dz), moved);
}

// Before player 2's run: what the game writes in the block outside the
// physics (runner, IDAS3_PHYSW) is the race's, not the car's, and comes from
// player 1's block: the race flags but bit 14 (the physics' own), the time
// step 0C901650, and the course queries' globals 0C9015D4-0C90171F. Word
// indexes in the block.
// The controls the physics reads (0C15CF56): the JVS analog channels as the
// game stores them, 16 bits each: steering, gas, brake. Player 2's pad goes
// there for its run, made as naomi_jvs.cpp makes player 1's.
const u32 kAnalogIn = 0x0C92F0E0;
u16 s_p1Analog[3];

u16 jvsAnalog(float v01)
{
    float x = v01 * 65536.f;
    u16 v = x < 0.f ? 0 : x > 65535.f ? 0xFFFF : (u16)x;
    if (v > 0xFF7F)
        v = 0xFF7F;
    if (v & 0x80)
        v += 0x100;
    return v;
}

void p2Controls()
{
    for (u32 i = 0; i < 3; i++)
        s_p1Analog[i] = ReadMem16(kAnalogIn + 2 * i);
    WriteMem16(kAnalogIn, jvsAnalog((s_pad.steer + 1.f) * 0.5f));
    WriteMem16(kAnalogIn + 2, jvsAnalog(s_pad.accel));
    WriteMem16(kAnalogIn + 4, jvsAnalog(s_pad.brake));
}

void p2RestoreControls()
{
    for (u32 i = 0; i < 3; i++)
        WriteMem16(kAnalogIn + 2 * i, s_p1Analog[i]);
}

const u32 kRaceFlags = 0x50 / 4;
const u32 kCarFlagBits = 1u << 14;
const u32 kRaceGlobals[][2] = {{0xD30 / 4, 1}, {0xDAC / 4, 1}, {0xDE4 / 4, 18}, {0xE6C / 4, 4}};

void p2Prepare()
{
    s_p2[kRaceFlags] = (s_p1[kRaceFlags] & ~kCarFlagBits) | (s_p2[kRaceFlags] & kCarFlagBits);
    for (const auto& g : kRaceGlobals)
        memcpy(&s_p2[g[0]], &s_p1[g[0]], g[1] * 4);
    p2Controls();
}

// After player 2's run, what the drivers update does after the AI for player
// 1 (0C159952-0C1599C8): with the four wheels on the road, the car's core
// state (0C900F00, 270 words) and render state are saved (0C9009F0,
// 0C8FF580); with three or four off it and the saved state on it, they are
// put back (the car never falls through the course).
const u32 kCore = 0x65C / 4, kCoreSaved = 0x14C / 4, kCoreWords = 270;
const u32 kWheels = 0x164 / 4;        // in the core and its saved copy: 4 words, surface in bits 4-7

u32 wheelsOff(const u32* w)
{
    u32 n = 0;
    for (u32 i = 0; i < 4; i++)
        n += (w[i] & 0xF0) == 0;
    return n;
}

void p2AfterRun()
{
    const u32 off = wheelsOff(&s_p2[kCore + kWheels]);
    if (off == 0)
    {
        memcpy(s_p2SavedRender, s_p2, sizeof(s_p2SavedRender));
        memcpy(&s_p2[kCoreSaved], &s_p2[kCore], kCoreWords * 4);
    }
    else if (off > 2 && wheelsOff(&s_p2[kCoreSaved + kWheels]) == 0)
    {
        memcpy(s_p2, s_p2SavedRender, sizeof(s_p2SavedRender));
        memcpy(&s_p2[kCore], &s_p2[kCoreSaved], kCoreWords * 4);
    }
}

// The CPU car's AI body where player 2's physics left the car: position,
// heading (the direction it moves, as the AI's: atan2(vx, vz)) and speed (km/h
// from the per-frame velocity). The render state's first nine words are the
// same in both kinds of car: position, velocity a frame, pitch yaw roll.
void p2SyncBody()
{
    writeWords(kCpuBody + kBodyPos, s_p2, 3);
    const float vx = wordFloat(s_p2[3]), vz = wordFloat(s_p2[5]);
    const float v = sqrtf(vx * vx + vz * vz);
    writeFloat(kCpuBody + kBodySpeed, v * 60.f * 3.6f);
    float heading = atan2f(vx, vz);
    if (v < 0.01f)
    {
        heading = wordFloat(s_p2[7]) - 3.14159265f;      // standing: the car's yaw
        if (heading < -3.14159265f)
            heading += 6.2831853f;
    }
    writeFloat(kCpuBody + kBodyHeading, heading);
    writeFloat(kCpuBody + kBodyYawRate, 0.f);
}

} // namespace

// The most of each control player 2 used since the last [2P] line (a pad that
// does not answer shows as zeros).
static float s_padMax[3];

void naomi_2pSetPad(const Naomi2pPad& pad)
{
    s_pad = pad;
    const float steer = pad.steer < 0.f ? -pad.steer : pad.steer;
    s_padMax[0] = pad.accel > s_padMax[0] ? pad.accel : s_padMax[0];
    s_padMax[1] = pad.brake > s_padMax[1] ? pad.brake : s_padMax[1];
    s_padMax[2] = steer > s_padMax[2] ? steer : s_padMax[2];
}

const Naomi2pPad& naomi_2pPad()
{
    return s_pad;
}

void naomi_2pVBlank()
{
    if (!g_two_player_preset)
        return;
    Race race;
    const bool on = findRace(race);
    if (!on)
    {
        if (s_active)
        {
            // Hand the camera back if its objects are still there (a change
            // of view, the end of the race).
            if (ReadMem32(s_race.rig) == kRigClass)
                followCar(s_race, 0);
            if (ReadMem32(kCameraPtr) == s_race.camera)
                setAspect(s_race, false);
            g_mirror_preset = s_mirrorSaved;
            naomi_Log("[2P] split screen off (no race camera)\n");
        }
        s_active = false;
        s_driving2 = false;
        return;
    }
    if (!s_active)
    {
        naomi_Log("[2P] split screen on: camera %08X rig %08X, player car %08X, CPU car %08X\n", race.camera,
                  race.rig, race.player, race.cpu);
        // One mirror for two cars would show the wrong one half the time,
        // and drawing the scene again is the in-car view's biggest cost.
        s_mirrorSaved = g_mirror_preset;
        g_mirror_preset = 0;
        s_active = true;
        // The views in flight are the player's; the cars start changing at
        // the next STARTRENDER.
        s_target[0] = s_target[1] = 0;
        followCar(race, 0);
    }
    s_race = race;
    setAspect(race, true);
    // FULL PHYSICS: player 2's car is made from player 1's once a race, on
    // the starting grid (or when split screen comes on later), and given up
    // when the race stops running.
    const u32 info = ReadMem32(kRaceInfo);
    const bool running = inRam(info) && (ReadMem32(info + 0x50) >> 15 & 1);
    if (s_raceRunning && !running)
        s_p2Ready = false;
    s_raceRunning = running;
    if (g_p2_car_preset && physicsCode())
    {
        if (!s_p2Ready)
            initP2();
    }
    else
        driveCpuCar(s_pad);
    if (++s_frames % 600 == 0)
    {
        if (g_p2_car_preset)
            naomi_Log("[2P] 10 s: views drawn %u left, %u right; player 2 full physics, %u runs, %.0f km/h; pad 2 "
                      "used up to gas %.2f, brake %.2f, steer %.2f\n",
                      s_drawn[0], s_drawn[1], s_p2Runs, readFloat(kCpuBody + kBodySpeed), s_padMax[0], s_padMax[1],
                      s_padMax[2]);
        else
            naomi_Log("[2P] 10 s: views drawn %u left, %u right; player 2 %s, %.0f km/h; pad 2 used up to gas %.2f, "
                      "brake %.2f, steer %.2f\n",
                      s_drawn[0], s_drawn[1], s_driving2 ? "driving" : "waiting for the start", s_speed2, s_padMax[0],
                      s_padMax[1], s_padMax[2]);
        s_p2Runs = 0;
        if (g_p2_scenery_preset)
        {
            const u32 view = findView(race);
            const u32 slot = view ? 12 * (ReadMem32(view + kViewSlot) & 1) : 0;
            naomi_Log("[2P]   course placed around player 2 for %u of its views (road segment %u, player 1's %u)\n",
                      s_courseSwaps, view ? ReadMem32(view + kViewOppSeg + slot) : 0,
                      view ? ReadMem32(view + kViewOwnSeg + slot) : 0);
        }
        s_courseSwaps = 0;
        s_drawn[0] = s_drawn[1] = 0;
        s_padMax[0] = s_padMax[1] = s_padMax[2] = 0.f;
    }
}

// STARTRENDER, before the renderer: this render is the scene the game built in
// its last frame, from the camera of the frame before (the top of the file).
// Then the rig goes to the other car for the frame starting now.
void naomi_2pStartRender()
{
    if (!s_active)
    {
        s_renderHalf = -1;
        return;
    }
    s_renderHalf = s_target[1];
    s_target[1] = s_target[0];
    s_target[0] = g_2p_hold >= 0 ? g_2p_hold : 1 - s_target[0];
    if (ReadMem32(s_race.rig) == kRigClass)
        followCar(s_race, s_target[0]);
}

// P2 SCENERY (the top of the file and kViewSetSegment): the course's update
// (r4 = the course) is about to follow the car at +50.
extern "C" u32 naomi_2pCourseEntry(u32 pc)
{
    return g_two_player_preset && g_p2_scenery_preset && (pc & 0x1FFFFFFF) == kCourseUpdate &&
           ReadMem16(pc) == 0x2F86 && ReadMem16(pc + 0x0E) == 0x5514;
}

extern "C" void naomi_2pCourse(u32 r4)
{
    if (!s_active || s_target[0] != 1 || !inRam(r4) || ReadMem32(r4) != kCourseClass)
        return;
    const u32 view = findView(s_race);
    if (view && ReadMem32(view + kViewCourse) == r4)
        WriteMem32(r4 + kCourseTarget, s_race.cpu + kCarMatrix);
}

// 1 at the view update's setSegment call (r12 = the view), 2 at its return.
extern "C" u32 naomi_2pViewEntry(u32 pc)
{
    if (!g_two_player_preset || !g_p2_scenery_preset)
        return 0;
    const u32 p = pc & 0x1FFFFFFF;
    if (p == kViewSetSegment)
        return ReadMem16(pc) == 0x908F && ReadMem16(pc + 0x14) == 0x051E && ReadMem16(pc + 0x20) == 0x06CE ? 1 : 0;
    if (p == kViewAfterSet)
        return ReadMem16(pc) == 0x4D0B ? 2 : 0;
    return 0;
}

extern "C" void naomi_2pView(u32 which, u32 r12)
{
    if (which == 2)
    {
        if (s_viewSwapped == r12)
            swapViewSegments(r12);
        s_viewSwapped = 0;
        return;
    }
    if (!s_active || s_target[0] != 1 || s_viewSwapped || r12 != findView(s_race))
        return;
    swapViewSegments(r12);
    s_viewSwapped = r12;
    s_courseSwaps++;
}

extern "C" int naomi_2pHalf()
{
    return s_renderHalf;
}

// The half of the scene being built, drawn by the next render.
extern "C" int naomi_2pNextHalf()
{
    return s_active ? s_target[1] : -1;
}

extern "C" void naomi_2pDrawn(int half)
{
    s_drawn[half & 1]++;
}

// Frameskip with split screen: the views alternate, so the renderers decide
// for a pair, the right view taking the left one's verdict; a regular skip
// rate on single views could drop the same car's view every time.
extern "C" bool naomi_2pSkip(int half, bool (*decide)())
{
    static bool pairSkip = false;
    if (half == 1)
        return pairSkip;
    const bool skip = decide();
    if (half == 0)
        pairSkip = skip;
    return skip;
}

// The HUD speed: the JIT (and the runner's interpreter) call naomi_2pHud(r14)
// at the entry of the block that reads the meter's speed for the HUD.
extern "C" u32 naomi_2pHudEntry(u32 pc)
{
    // The block, and the two reads further on, as the hook expects them.
    return g_two_player_preset && (pc & 0x1FFFFFFF) == kHudBlock && ReadMem16(pc) == 0x51E2 &&
           ReadMem16(pc + 0x0E) == 0x7258 && ReadMem16(pc + 0x10) == 0xF428 && ReadMem16(pc + 0x134) == 0x7A54 &&
           ReadMem16(pc + 0x138) == 0xF1A8;
}

// The HUD drawn in a frame reaches the screen a render later than the scene
// built in that frame (runner snapshots: with [1] the two speeds came out
// swapped), so it belongs with the car the rig was just given, [0].
// FULL PHYSICS: 1 at the return from the player's physics, 2 at the return
// from the CPU car's AI (the CPU back ends call naomi_2pPhys(which) there and
// jump to what it returns, if not 0).
extern "C" u32 naomi_2pPhysEntry(u32 pc)
{
    if (!g_two_player_preset || !g_p2_car_preset)
        return 0;
    const u32 p = pc & 0x1FFFFFFF;
    if (p != kAfterPhysics && p != kAfterAi)
        return 0;
    if (!physicsCode())
        return 0;
    return p == kAfterPhysics ? 1 : 2;
}

extern "C" u32 naomi_2pPhys(u32 which)
{
    if (!s_active || !s_p2Ready)
        return 0;
    if (which == 1)
    {
        if (!s_inP2Run)
        {
            // Player 1's physics is done: player 2's turn. The opponent it
            // sees in the collision probes (built from the render states by
            // 0C157880) is player 1.
            readWords(kBlockLo, s_p1, kBlockWords);
            readOutside(s_p1Out);
            readWords(kProbes, s_probes, kProbeWords);
            readWords(kRenderP1, s_slots, 2 * kSlotWords);
            p2Prepare();
            writeWords(kBlockLo, s_p2, kBlockWords);
            writeOutside(s_p2Out);
            writeWords(kRenderP1, s_p2, kRenderWords);
            writeWords(kRenderCpu, s_p1, kRenderCarWords);
            s_inP2Run = true;
            return kRerun;
        }
        // Player 2's physics is done: back to player 1's state.
        readWords(kBlockLo, s_p2, kBlockWords);
        readOutside(s_p2Out);
        p2AfterRun();
        p2RestoreControls();
        writeWords(kBlockLo, s_p1, kBlockWords);
        writeOutside(s_p1Out);
        writeWords(kRenderP1, s_slots, 2 * kSlotWords);
        writeWords(kProbes, s_probes, kProbeWords);
        s_inP2Run = false;
        s_p2Runs++;
        return 0;
    }
    // After the AI: the CPU car is player 2's (0C157A80 copies this to its
    // render state next), and the AI's body follows it, so its course
    // tracking (position in the race) and the CPU car's sounds do too.
    writeWords(kRenderCpuCopy, s_p2, kRenderCarWords);
    p2SyncBody();
    return 0;
}

extern "C" void naomi_2pHud(u32 r14)
{
    if (!s_active || s_target[0] != 1 || !inRam(r14))
        return;
    const u32 meter = ReadMem32(r14 + 4);
    if (!inRam(meter) || ReadMem32(meter) != kMeterClass)
        return;
    if (g_p2_car_preset && s_p2Ready)
    {
        // Player 2's own car: its speed and the rev counter from its vehicle.
        writeFloat(meter + kMeterSpeed, wordFloat(s_p2Out[kVehicleSpeed]) * 3.6f);
        WriteMem32(meter + kMeterRevs, s_p2Out[kVehicleRevs]);
    }
    else
        writeFloat(meter + kMeterSpeed, readFloat(kCpuBody + kBodySpeed));
}
