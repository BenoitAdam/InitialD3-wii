// MOTION FX (launcher RENDERING 2 page; the effects are drawn by gxRend.cpp,
// mfx_*): how the camera of a race view moves, so the renderer can smooth a
// race shown at 30 frames a second. Speed blur and in-between frames scale
// with the speed, and the in-between frames move the last picture on by one
// frame of travel and turning.
//
// What is read (GDS-0033, found for 2 PLAYERS, naomi_2p.cpp): both cars'
// render states start with nine floats, position, velocity a frame (metres)
// and pitch, yaw, roll (radians, the yaw growing to the driver's left): the
// player's at 0C8FF388, the CPU car's at 0C8FF430 (player 2's car in 2
// PLAYERS). Sampled at every vblank, and only in a race view (the race camera
// on a car-follow rig: no menus, replays or fly-bys).
#include "types.h"
#include "dc/mem/sh4_mem.h"
#include "naomi.h"
#include "naomi_elan.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
int g_motion_fx_preset = 0;      // 0 OFF, 1 SPEED BLUR, 2 FRAME BLEND, 3 IN-BETWEEN, 4 BLUR + IN-BETWEEN
int g_motion_level_preset = 2;   // 1 LOW, 2 MEDIUM, 3 HIGH
int g_between_warp_preset = 2;   // 0 SMOOTH, 1 BANDS, 2 ROTATION ONLY, 3 NO MOVE
bool g_motion_trace_now = false; // runner: this vblank is in IDAS3_MOTION's range
}

namespace {

const u32 kRenderState[2] = {0x0C8FF388, 0x0C8FF430};   // the player's car, the CPU car

struct Car
{
    float yaw;
    float step;        // metres a frame
    float yawStep;     // radians a frame, + to the driver's left
    bool have;         // yaw read last vblank
};
Car s_car[2];
bool s_raceView = false;
u32 s_frame = 0;
// The in-car view, told by its rear-view mirror (models with user clip mode
// 2, counted even when MIRROR OFF drops them: g_elan_mirror_seen), as IN-CAR
// REFLECT does: up for 3 vblanks after the last one.
u32 s_mirrorLast = 0, s_inCar = 0;

// The scene's projection: a race frame sets three (the scene's, the rear-view
// mirror's and the HUD's), so the one most vertices are drawn with since the
// last vblank. |fx| in screen pixels (640 wide).
struct Projection
{
    float fx;
    u32 vertices;
};
Projection s_proj[4];
u32 s_nProj = 0;
float s_curFx = 0.f;
float s_focal = 0.f;

// Runner (env IDAS3_MOTION=FROM:TO): a [motion] line a vblank in those
// frames, and the projections the ELAN is given.
int s_traceFrom = -1, s_traceTo = -1;

float wordFloat(u32 w)
{
    float f;
    memcpy(&f, &w, 4);
    return f;
}

bool sane(float f, float limit)
{
    return f == f && f > -limit && f < limit;
}

} // namespace

extern "C" bool naomi_RaceView();   // naomi_2p.cpp

void naomi_MotionVBlank()
{
    s_frame++;
#if !defined(GEKKO)
    if (s_traceFrom == -1)
    {
        s_traceFrom = -2;
        if (const char* e = getenv("IDAS3_MOTION"))
            sscanf(e, "%d:%d", &s_traceFrom, &s_traceTo);
    }
#endif
    const bool traced = g_motion_trace_now;   // the frame that just ended
    g_motion_trace_now = (int)s_frame >= s_traceFrom && (int)s_frame <= s_traceTo;
    if (!g_motion_fx_preset && !g_motion_trace_now && !traced)
        return;
    u32 best = 0;
    for (u32 i = 0; i < s_nProj; i++)
    {
        if (traced)
            printf("[motion]   projection fx %.2f: %u vertices\n", s_proj[i].fx, s_proj[i].vertices);
        if (s_proj[i].vertices > best)
        {
            best = s_proj[i].vertices;
            s_focal = fabsf(s_proj[i].fx);
        }
    }
    s_nProj = 0;
    if (g_elan_mirror_seen != s_mirrorLast)
    {
        s_mirrorLast = g_elan_mirror_seen;
        s_inCar = 3;
    }
    else if (s_inCar)
        s_inCar--;
    s_raceView = naomi_RaceView();
    for (int c = 0; c < 2; c++)
    {
        Car& car = s_car[c];
        if (!s_raceView)
        {
            car.have = false;
            car.step = car.yawStep = 0.f;
            continue;
        }
        const u32 a = kRenderState[c];
        const float vx = wordFloat(ReadMem32(a + 12)), vy = wordFloat(ReadMem32(a + 16)),
                    vz = wordFloat(ReadMem32(a + 20));
        const float yaw = wordFloat(ReadMem32(a + 28));
        if (!sane(vx, 10.f) || !sane(vy, 10.f) || !sane(vz, 10.f) || !sane(yaw, 100.f))
        {
            car.have = false;
            car.step = car.yawStep = 0.f;
            continue;
        }
        car.step = sqrtf(vx * vx + vy * vy + vz * vz);
        float d = 0.f;
        if (car.have)
        {
            d = yaw - car.yaw;
            while (d > 3.14159265f)
                d -= 6.2831853f;
            while (d < -3.14159265f)
                d += 6.2831853f;
            // A respawn or a camera reset is not a turn.
            if (d > 0.2f || d < -0.2f)
                d = 0.f;
        }
        car.yawStep = d;
        car.yaw = yaw;
        car.have = true;
    }
    if (g_motion_trace_now)
        printf("[motion] f%u race view %d, in-car %d | player %.3f m a frame (%.0f km/h), yaw %+.5f a frame | CPU car "
               "%.3f m, %+.5f | focal %.1f\n",
               s_frame, s_raceView ? 1 : 0, s_inCar ? 1 : 0, s_car[0].step, s_car[0].step * 216.f, s_car[0].yawStep,
               s_car[1].step, s_car[1].yawStep, s_focal);
}

// The ELAN's projection command (naomi_elan_tnl.cpp): screen x = fx X / Z + tx.
extern "C" void naomi_MotionProjection(float fx, float, float, float)
{
    s_curFx = fx;
}

// Each list the ELAN draws (or would, on a dropped frame), under the last
// projection set.
extern "C" void naomi_MotionList(u32 vertices)
{
    for (u32 i = 0; i < s_nProj; i++)
        if (s_proj[i].fx == s_curFx)
        {
            s_proj[i].vertices += vertices;
            return;
        }
    if (s_nProj < 4)
        s_proj[s_nProj++] = {s_curFx, vertices};
}

extern "C" bool naomi_Motion(int half, NaomiMotion* m)
{
    const Car& car = s_car[half == 1 ? 1 : 0];
    m->valid = s_raceView && car.have;
    m->step = m->valid ? car.step : 0.f;
    m->yawStep = m->valid ? car.yawStep : 0.f;
    m->kmh = m->step * 216.f;
    m->focal = s_focal;
    m->inCar = s_inCar != 0;
    return m->valid;
}
