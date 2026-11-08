// The mouse look test (testing, $TWIN_LOOK_TEST=1, with --quick): from the beach, a few seconds' play, then the mouse moved right at
// a steady 3000 points a second (mouse look holding it without a window) for each of the cases below, the follow camera's turn
// measured over a second of its clock (its yaw, ScaleCameraTurn's) after half a second to settle, then a second still. The
// defaults (a mouse sensitivity of 5, a camera speed of 5) turn it at the stick's full deflection, the game's fastest (120 degrees
// a second); a sensitivity of 10 six times as fast, a camera speed of 10 twice; all the same way. "look test: pass" and exit 0,
// else what failed and 1; a step taking over 30 s fails it
#include "ui/bindings.h"

#include "native.h"

#include "game/clock.h"
#include "game/gamecontroller.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace NativeUi
{
namespace
{
using Clock = std::chrono::steady_clock;

struct Case
{
    s32 sensitivity;
    s32 cameraSpeed;
    // The turn expected, times the defaults'
    f32 times;
};

constexpr Case Cases[] = {{5, 5, 1.0f}, {10, 5, 6.0f}, {5, 10, 2.0f}};
constexpr u32 CaseCount = sizeof(Cases) / sizeof(Cases[0]);
constexpr f32 MotionSpeed = 3000.0f;
constexpr f32 PlaySeconds = 3.0f;
constexpr f32 SettleSeconds = 0.5f;
constexpr f32 MeasureSeconds = 1.0f;
constexpr f32 StillSeconds = 1.0f;
// The stick's full deflection's turn (the game's g_RigYawInputSpeed) and how far off a measure may be
constexpr f32 StickDegrees = 120.0f;
constexpr f32 Tolerance = 0.1f;

enum class Phase
{
    Playing,
    Settling,
    Measuring,
    Still,
};

bool g_Read = false;
bool g_Active = false;
Phase g_Phase = Phase::Playing;
u32 g_Case = 0;
bool g_Playing = false;
Clock::time_point g_StepStart;
Clock::time_point g_LastFeed;
// The camera's yaw and clock last frame, and the measure's sums (65536ths of a turn, clock units)
bool g_HaveCamera = false;
s32 g_LastYaw = 0;
u32 g_LastTime = 0;
s64 g_TurnSum = 0;
u64 g_TimeSum = 0;
f32 g_Degrees[CaseCount] = {};

f32 Since(Clock::time_point start)
{
    return std::chrono::duration<f32>(Clock::now() - start).count();
}

[[noreturn]] void Finish(int status, const char* why)
{
    Native::Log("look test: %s", why);
    std::exit(status);
}

void Next(Phase phase)
{
    g_Phase = phase;
    g_StepStart = Clock::now();
}

void Start(u32 index)
{
    g_Case = index;
    SetOption(OptionMouseSensitivity, MouseSensitivityStep(static_cast<f32>(Cases[index].sensitivity)));
    SetOption(OptionCameraSpeed, Cases[index].cameraSpeed);
    g_LastFeed = Clock::now();
    Next(Phase::Settling);
}

void Judge()
{
    f32 base = g_Degrees[0];
    if (std::fabs(std::fabs(base) - StickDegrees) > StickDegrees * Tolerance)
    {
        Finish(1, "the defaults don't turn the camera at the stick's full deflection");
    }

    for (u32 i = 1; i < CaseCount; i++)
    {
        f32 times = g_Degrees[i] / base;
        if (std::fabs(times - Cases[i].times) > Cases[i].times * Tolerance)
        {
            char why[96];
            std::snprintf(why, sizeof(why), "sensitivity %d, camera speed %d: %.2f times the defaults' turn, not %.0f",
                          Cases[i].sensitivity, Cases[i].cameraSpeed, times, Cases[i].times);
            Finish(1, why);
        }
    }

    Finish(0, "pass");
}
}

bool LookTestActive()
{
    if (!g_Read)
    {
        g_Read = true;
        const char* test = std::getenv("TWIN_LOOK_TEST");
        g_Active = test != nullptr && test[0] != '\0';
        g_StepStart = Clock::now();
    }

    return g_Active;
}

void LookTestFrame()
{
    if (!LookTestActive() || G_GameController == nullptr)
    {
        return;
    }

    if (Since(g_StepStart) > 30.0f)
    {
        char why[64];
        std::snprintf(why, sizeof(why), "stuck (step %u, case %u)", static_cast<u32>(g_Phase), g_Case);
        Finish(1, why);
    }

    // Mouse look holds the mouse only in play: the cases are measured there
    bool looking = MouseLookActive();
    if (g_Phase != Phase::Playing && !looking)
    {
        Finish(1, "play stopped during a case");
    }

    // The mouse moved right at a steady speed while settling and measuring
    if (g_Phase == Phase::Settling || g_Phase == Phase::Measuring)
    {
        Clock::time_point now = Clock::now();
        MouseLookTestMotion(MotionSpeed * std::chrono::duration<f32>(now - g_LastFeed).count(), 0.0f);
        g_LastFeed = now;
    }

    switch (g_Phase)
    {
    case Phase::Playing:
        if (!looking)
        {
            g_Playing = false;
        }
        else if (!g_Playing)
        {
            g_Playing = true;
            g_StepStart = Clock::now();
        }
        else if (Since(g_StepStart) > PlaySeconds)
        {
            Start(0);
        }

        break;
    case Phase::Settling:
        if (Since(g_StepStart) > SettleSeconds)
        {
            g_TurnSum = 0;
            g_TimeSum = 0;
            Next(Phase::Measuring);
        }

        break;
    case Phase::Measuring:
        if (static_cast<f32>(g_TimeSum) * g_SecondsPerClockUnit >= MeasureSeconds)
        {
            f32 seconds = static_cast<f32>(g_TimeSum) * g_SecondsPerClockUnit;
            g_Degrees[g_Case] = static_cast<f32>(g_TurnSum) * 360.0f / 65536.0f / seconds;
            Native::Log("look test: sensitivity %d, camera speed %d: %.1f degrees a second", Cases[g_Case].sensitivity,
                        Cases[g_Case].cameraSpeed, g_Degrees[g_Case]);
            Next(Phase::Still);
        }

        break;
    case Phase::Still:
        if (Since(g_StepStart) > StillSeconds)
        {
            if (g_Case + 1 < CaseCount)
            {
                Start(g_Case + 1);
            }
            else
            {
                Judge();
            }
        }

        break;
    }
}

void LookTestCamera(s32 yaw, u32 time)
{
    if (!LookTestActive())
    {
        return;
    }

    if (g_HaveCamera && g_Phase == Phase::Measuring)
    {
        // The turn since last frame the short way round
        g_TurnSum += static_cast<s16>(static_cast<u16>(yaw - g_LastYaw));
        g_TimeSum += time - g_LastTime;
    }

    g_HaveCamera = true;
    g_LastYaw = yaw;
    g_LastTime = time;
}
}
