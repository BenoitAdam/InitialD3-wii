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
// - The CPU car is not the player's physics: its motion body (0C901F38, code
//   0C15B000-0C15CFFF) moves along its heading (+AC, radians) at its speed
//   (+44, km/h) every frame. Its AI aims at a point of its racing line ahead
//   (the heading error feeds a damped yaw rate) and changes the speed by a
//   bounded step, holding it at 0 until the race runs (bit 15 of
//   [[0C900954]+50]). Player 2 sets the speed and nudges the heading at every
//   vblank. The AI's pull toward its line balances the nudge: holding the
//   stick moves the car across the road, letting go brings it back. The car
//   stays on the course (a free heading drives through the guardrails).
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
int s_camera = 0;          // car the camera follows in the frame being built: 0 player, 1 CPU
int s_renderHalf = -1;     // half of the render in flight, -1 whole picture
int s_lastDrawn = -1;      // half of the last view that reached the screen
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

void naomi_2pSetPad(const Naomi2pPad& pad)
{
    s_pad = pad;
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
        s_lastDrawn = -1;
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
    }
    s_race = race;
    // The car whose half was not drawn last goes next, so a frame the
    // frameskip drops is shown by the following one.
    s_camera = s_lastDrawn == 0 ? 1 : 0;
    followCar(race, s_camera);
    setAspect(race, true);
    driveCpuCar(s_pad);
    if (++s_frames % 600 == 0)
    {
        naomi_Log("[2P] 10 s: views drawn %u left, %u right; player 2 %s, %.0f km/h\n", s_drawn[0], s_drawn[1],
                  s_driving2 ? "driving" : "waiting for the start", s_speed2);
        s_drawn[0] = s_drawn[1] = 0;
    }
}

void naomi_2pStartRender()
{
    s_renderHalf = s_active ? s_camera : -1;
}

extern "C" int naomi_2pHalf()
{
    return s_renderHalf;
}

extern "C" int naomi_2pNextHalf()
{
    return s_active ? 1 - s_renderHalf : -1;
}

extern "C" void naomi_2pDrawn(int half)
{
    s_lastDrawn = half;
    s_drawn[half & 1]++;
}
