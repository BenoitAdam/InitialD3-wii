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
//   drawn, at 0C0C9BD0 (block 0C0C9BC8, meter = [r14+4]): the HUD of the CPU
//   car's view is given that car's speed there (naomi_2pHud). That HUD is
//   shown two renders later, the 3D scene built in the same frame one.
// - The CPU car is not the player's physics: its motion body (0C901F38, code
//   0C15B000-0C15CFFF) moves along its heading (+AC, radians) at its speed
//   (+44, km/h) every frame. Its AI aims at a point of its racing line ahead
//   (the heading error feeds a damped yaw rate) and changes the speed by a
//   bounded step, holding it at 0 until the race runs (bit 15 of
//   [[0C900954]+50]). Player 2 sets the speed and nudges the heading at every
//   vblank. The AI's pull toward its line balances the nudge: holding the
//   stick moves the car across the road, letting go brings it back. The car
//   stays on the course (a free heading drives through the guardrails).
//
// Not done yet (owner, 2026-11-07), what is known so far:
// - Player 2 in full control. The player's physics is one routine run by the
//   drivers update 0C159920 (fr4 = dt -> 0C901650): 0C157880 (both cars'
//   collision probes 0C401B04 / 0C401D74, 0x270 each, built from the render
//   states 0C8FF388 / 0C8FF430; car-car collision 0C0C06C0), then 0C157AE0
//   (record = [0C900954] = 0C9008A4; simulation 0C15CEC0 on the core state
//   0C900F00; copies it back into the record; 0C157A80 copies the record's
//   first 80 bytes to the render state 0C8FF388, and the CPU copy 0C9017D4 to
//   0C8FF430), then the CPU car's AI 0C15B0A0(1, 1). The physics code also
//   names ~50 record fields by address (0C9008A4-0C901720), so switching the
//   pointer is not enough: the plan is a second run a frame with that block
//   swapped (and the render states / probes swapped so the opponent is player
//   1), player 2's controls put where the physics reads them (not found yet),
//   and its result written to 0C9017D4 after the AI. Statics outside the block
//   it touches (wheel counters at 0CAA94B4, tire marks, sound) are unchecked.
// - Scenery for player 2 far from player 1: from ~800 m apart (runner: fine at
//   113 m, gone at 922 m) player 2's view keeps only the far backdrop. The
//   choice of course blocks follows player 1, not the camera; the per-car
//   course tracker around 0C069480 (virtual calls on a course object with each
//   car's position) is the next place to look.
#include "types.h"
#include "dc/mem/sh4_mem.h"
#include "naomi.h"
#include "naomi_elan.h"

#include <cstring>

extern "C" {
int g_two_player_preset = 0;          // 0 OFF, 1 SIDE BY SIDE
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
const u32 kBodyPos = 0xC8;            // x y z, as the state record's
const u32 kRaceInfo = 0x0C900954;     // -> the race's flags (+50, bit 15: running)
const u32 kHudSpeedBlock = 0x0C0C9BC8; // HUD draw: r10 = [r14+4] + 54, then reads the speed
const u32 kMeterClass = 0x0C3873C4;
const u32 kMeterSpeed = 0x54;         // km/h, what the HUD shows (truncated)

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
    driveCpuCar(s_pad);
    if (++s_frames % 600 == 0)
    {
        naomi_Log("[2P] 10 s: views drawn %u left, %u right; player 2 %s, %.0f km/h; pad 2 used up to gas %.2f, "
                  "brake %.2f, steer %.2f\n",
                  s_drawn[0], s_drawn[1], s_driving2 ? "driving" : "waiting for the start", s_speed2, s_padMax[0],
                  s_padMax[1], s_padMax[2]);
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
    s_target[0] = 1 - s_target[0];
    if (ReadMem32(s_race.rig) == kRigClass)
        followCar(s_race, s_target[0]);
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
    return g_two_player_preset && (pc & 0x1FFFFFFF) == kHudSpeedBlock && ReadMem16(pc) == 0x5AE1 &&
           ReadMem16(pc + 4) == 0x7A54 && ReadMem16(pc + 8) == 0xF1A8;
}

// The HUD drawn in a frame reaches the screen a render later than the scene
// built in that frame (runner snapshots: with [1] the two speeds came out
// swapped), so it belongs with the car the rig was just given, [0].
extern "C" void naomi_2pHud(u32 r14)
{
    if (!s_active || s_target[0] != 1 || !inRam(r14))
        return;
    const u32 meter = ReadMem32(r14 + 4);
    if (!inRam(meter) || ReadMem32(meter) != kMeterClass)
        return;
    writeFloat(meter + kMeterSpeed, readFloat(kCpuBody + kBodySpeed));
}
