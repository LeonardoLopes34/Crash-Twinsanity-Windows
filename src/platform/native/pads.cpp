// The controllers: the machine's gamepad (SDL3) and keyboard, presented to the game as a DualShock 2 on port 0 the way the PS2's
// libpad does (analog mode, two actuators, pressure sensitive mode), reporting what the PS2's report has: the button word
// (pressed 0), the sticks (0x80 the middle, up 0) and the buttons' pressures (0-255). See native/NATIVE.md for the mapping
#include "native.h"
#include "fixes.h"
#include "coop/coop.h"
#include "platform/pads.h"
#include "ui/bindings.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace
{
using namespace Platform::Pads;

// The report's button word, pressed 1 before it's inverted (the PS2's bits: high byte left, down, right, up, start, R3, L3,
// select; low byte square, cross, circle, triangle, R1, L1, R2, L2)
enum Button : u32
{
    ButtonL2 = 0x1,
    ButtonR2 = 0x2,
    ButtonL1 = 0x4,
    ButtonR1 = 0x8,
    ButtonTriangle = 0x10,
    ButtonCircle = 0x20,
    ButtonCross = 0x40,
    ButtonSquare = 0x80,
    ButtonSelect = 0x100,
    ButtonL3 = 0x200,
    ButtonR3 = 0x400,
    ButtonStart = 0x800,
    ButtonUp = 0x1000,
    ButtonRight = 0x2000,
    ButtonDown = 0x4000,
    ButtonLeft = 0x8000,
};

// A DualShock 2 in pressure sensitive mode: type 7 (analog), 9 halfwords of data after the header
constexpr u8 ModeAnalog = 0x73;
constexpr u8 ModePressure = 0x79;
constexpr s32 PressureReportBytes = 20;
constexpr s32 AnalogReportBytes = 8;
constexpr u8 StickMiddle = 0x80;
constexpr u8 Pressed = 0xFF;

bool g_Initialised = false;
bool g_Open = false;
bool g_PressureMode = false;
SDL_Gamepad* g_Gamepad = nullptr;
// The second player's (coop/secondplayer.cpp): port 1, the second gamepad plugged in (a friend's through Parsec)
bool g_SecondOpen = false;
SDL_Gamepad* g_SecondGamepad = nullptr;

// What each button's pressure byte is (the PS2's ReportByte order)
struct PressureOf
{
    Button button;
    ReportByte byte;
};
constexpr PressureOf Pressures[] = {
    {ButtonRight, PressureRight}, {ButtonLeft, PressureLeft},     {ButtonUp, PressureUp},       {ButtonDown, PressureDown},
    {ButtonTriangle, PressureTriangle}, {ButtonCircle, PressureCircle}, {ButtonCross, PressureCross},
    {ButtonSquare, PressureSquare}, {ButtonL1, PressureL1},       {ButtonR1, PressureR1},       {ButtonL2, PressureL2},
    {ButtonR2, PressureR2},
};

void StartSdl()
{
    if (!g_Initialised)
    {
        g_Initialised = true;
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS))
        {
            Native::Log("pads: SDL's gamepads didn't start: %s", SDL_GetError());
        }
    }
}

// The first gamepad plugged in, kept until it's unplugged
void FindGamepad()
{
    if (g_Gamepad != nullptr && SDL_GamepadConnected(g_Gamepad))
    {
        return;
    }

    if (g_Gamepad != nullptr)
    {
        SDL_CloseGamepad(g_Gamepad);
        g_Gamepad = nullptr;
    }

    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    // Two players on this PC with a single gamepad: it's the second player's (a friend's through Parsec), the first plays on the
    // keyboard; with two or more, the first is the first player's
    if (ids != nullptr && count == 1 && Coop::SecondPlayerWanted())
    {
        SDL_free(ids);
        return;
    }

    if (ids != nullptr && count > 0)
    {
        g_Gamepad = SDL_OpenGamepad(ids[0]);
        if (g_Gamepad != nullptr)
        {
            Native::Log("pads: %s", SDL_GetGamepadName(g_Gamepad));
        }
    }

    SDL_free(ids);
}

// The second gamepad: the first one plugged in that isn't the first player's, kept until it's unplugged
void FindSecondGamepad()
{
    if (g_SecondGamepad != nullptr && SDL_GamepadConnected(g_SecondGamepad) && g_SecondGamepad != g_Gamepad)
    {
        return;
    }

    if (g_SecondGamepad != nullptr && g_SecondGamepad != g_Gamepad)
    {
        SDL_CloseGamepad(g_SecondGamepad);
    }

    g_SecondGamepad = nullptr;
    SDL_JoystickID firstId = g_Gamepad != nullptr ? SDL_GetGamepadID(g_Gamepad) : 0;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int index = 0; ids != nullptr && index < count; index++)
    {
        if (ids[index] != firstId)
        {
            g_SecondGamepad = SDL_OpenGamepad(ids[index]);
            if (g_SecondGamepad != nullptr)
            {
                Native::Log("pads: the second player's: %s", SDL_GetGamepadName(g_SecondGamepad));
            }

            break;
        }
    }

    SDL_free(ids);
}
}

s32 Platform::Pads::Initialise()
{
    StartSdl();
    return 1;
}

s32 Platform::Pads::Open(s32 port, s32 slot, void*)
{
    StartSdl();
    if (port == 1 && slot == 0)
    {
        // The second player's port (always there: what plays it is looked for each frame)
        g_SecondOpen = true;
        return 1;
    }

    if (port != 0 || slot != 0)
    {
        return 0;
    }

    g_Open = true;
    return 1;
}

s32 Platform::Pads::Close(s32 port, s32 slot)
{
    if (port == 0 && slot == 0)
    {
        g_Open = false;
        g_PressureMode = false;
    }
    else if (port == 1 && slot == 0)
    {
        g_SecondOpen = false;
    }

    return 1;
}

Platform::Pads::State Platform::Pads::GetState(s32 port, s32 slot)
{
    if (port == 1 && slot == 0)
    {
        return g_SecondOpen ? StateStable : StateDisconnected;
    }

    return port == 0 && slot == 0 && g_Open ? StateStable : StateDisconnected;
}

Platform::Pads::RequestState Platform::Pads::GetRequestState(s32, s32)
{
    return RequestComplete;
}

// A DualShock 2's modes: digital (4) and analog (7), in analog mode
s32 Platform::Pads::InfoMode(s32 port, s32 slot, ModeInfo info, s32 index)
{
    if (GetState(port, slot) != StateStable)
    {
        return 0;
    }

    switch (info)
    {
    case InfoCurrentId:
    case InfoCurrentExtendedId:
        return ModeIdAnalog;
    case InfoCurrentIndex:
        return 1;
    case InfoModeTable:
        return index == ModeTableLength ? 2 : index == 0 ? ModeIdDigital : index == 1 ? ModeIdAnalog : 0;
    }

    return 0;
}

s32 Platform::Pads::SetMainMode(s32, s32, MainMode, ModeLock)
{
    return 1;
}

// Two actuators, as a DualShock 2 has: the small motor (on or off) and the large one (0-255)
s32 Platform::Pads::InfoActuator(s32 port, s32 slot, s32 actuator, s32)
{
    if (GetState(port, slot) != StateStable)
    {
        return 0;
    }

    return actuator == CountActuators ? 2 : 1;
}

s32 Platform::Pads::SetActuatorAlign(s32, s32, const u8*)
{
    return 1;
}

s32 Platform::Pads::SetActuatorDirect(s32 port, s32 slot, const u8 values[6])
{
    if (port != 0 || slot != 0)
    {
        return 0;
    }

    if (g_Gamepad != nullptr)
    {
        // The small motor on or off, the large one by its strength; the game sets them again every frame it wants them
        u16 small = values[0] != 0 ? 0xFFFF : 0;
        u16 large = static_cast<u16>(values[1] * 0x101);
        SDL_RumbleGamepad(g_Gamepad, large, small, 100);
    }

    return 1;
}

s32 Platform::Pads::InfoPressureMode(s32 port, s32 slot)
{
    return GetState(port, slot) == StateStable ? 1 : 0;
}

s32 Platform::Pads::EnterPressureMode(s32 port, s32 slot)
{
    if (GetState(port, slot) != StateStable)
    {
        return 0;
    }

    g_PressureMode = true;
    return 1;
}

namespace
{
// Keys pressed since the last read: a press shorter than a frame (the native frames can be slow) still counts for one
bool g_Latched[SDL_SCANCODE_COUNT];
bool g_Watching = false;
bool g_QuitAsked = false;

bool WatchPadEvents(void*, SDL_Event* event)
{
    if (event->type == SDL_EVENT_KEY_DOWN && event->key.scancode > SDL_SCANCODE_UNKNOWN &&
        event->key.scancode < SDL_SCANCODE_COUNT)
    {
        g_Latched[event->key.scancode] = true;
    }
    else if (event->type == SDL_EVENT_QUIT || event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
    {
        // The window's close button, the Dock's or the menu's quit, Cmd+Q
        g_QuitAsked = true;
    }

    return true;
}
}

namespace
{
// A report as the PS2's pad gives it, from what the bindings made of the keys and a gamepad
s32 WriteReport(const NativeUi::BoundPad& bound, u8 leftX, u8 leftY, u8* data)
{
    u32 buttons = bound.buttons;
    u8 pressure[PressureReportBytes] = {};
    pressure[PressureL2] = bound.l2Pressure;
    pressure[PressureR2] = bound.r2Pressure;
    for (const PressureOf& of : Pressures)
    {
        if ((buttons & of.button) != 0 && pressure[of.byte] == 0)
        {
            pressure[of.byte] = Pressed;
        }
    }

    std::memset(data, 0, PressureReportBytes);
    data[ReportMode] = g_PressureMode ? ModePressure : ModeAnalog;
    data[ReportButtonsHigh] = static_cast<u8>(~buttons >> 8);
    data[ReportButtonsLow] = static_cast<u8>(~buttons);
    data[StickRightX] = bound.rightX;
    data[StickRightY] = bound.rightY;
    data[StickLeftX] = leftX;
    data[StickLeftY] = leftY;
    if (g_PressureMode)
    {
        for (u32 byte = PressureRight; byte <= PressureR2; byte++)
        {
            data[byte] = pressure[byte];
        }
    }

    return g_PressureMode ? PressureReportBytes : AnalogReportBytes;
}

// The second player's report: the second gamepad's buttons and sticks by the same bindings (no keyboard: the first player has it).
// $TWIN_P2_TEST_RUN=seconds (tests): that long after the game starts, its left stick turned round on its own
s32 ReadSecond(u8* data)
{
    if (!g_SecondOpen)
    {
        return 0;
    }

    FindSecondGamepad();
    NativeUi::BoundPad bound;
    NativeUi::ReadGamepadOnly(g_SecondGamepad, &bound);
    u8 leftX = bound.leftX;
    u8 leftY = bound.leftY;
    static const char* testRun = std::getenv("TWIN_P2_TEST_RUN");
    if (testRun != nullptr)
    {
        double seconds = static_cast<double>(SDL_GetTicks()) / 1000.0 - std::atof(testRun);
        if (seconds > 0 && std::fmod(seconds, 4.0) >= 1.0)
        {
            double angle = seconds * (2.0 * 3.14159265 / 5.0);
            leftX = static_cast<u8>(128.0 + 127.0 * std::cos(angle));
            leftY = static_cast<u8>(128.0 + 127.0 * std::sin(angle));
        }
    }

    return WriteReport(bound, leftX, leftY, data);
}
}

s32 Platform::Pads::Read(s32 port, s32 slot, u8* data)
{
    if (!g_Watching)
    {
        g_Watching = true;
        SDL_AddEventWatch(WatchPadEvents, nullptr);
    }

    if (port == 1 && slot == 0)
    {
        return ReadSecond(data);
    }

    if (port != 0 || slot != 0 || !g_Open)
    {
        return 0;
    }

    SDL_PumpEvents();
    if (g_QuitAsked)
    {
        // Quitting: the saves and settings are written as they're made, so nothing is left to keep
        // Straight out: SDL's shutdown under the sound thread still feeding its stream crashed on the way out
        Native::EndSession("quit: the window closed");
        std::_Exit(0);
    }

    Native::SnapshotFrame();
    Native::QuickStartFrame();
    NativeFixes::Frame();
    NativeFixes::GiveBackFrame();
    // Online co-op's frame (coop/coopgame.cpp): here too, so the network keeps going while a cutscene plays
    Coop::GameFrame();
    // The free camera's key (F7), with or without co-op
    Coop::CameraKeys();
    // The second player on this PC (coop/secondplayer.cpp)
    Coop::SecondPlayerFrame();
    // The level sweep's frame here too: GameController::Update (its other caller) doesn't run while some cutscenes play, the pads'
    // reads do (their skip polls them)
    NativeUi::SweepFrame(true);
    FindGamepad();
    // The keyboard's keys and the gamepad's buttons by the controls' bindings (src/platform/native/ui/bindings.cpp: the defaults
    // are NATIVE.md's), the sticks the gamepad's (the keys bound to the left stick's directions over it)
    NativeUi::BoundPad bound;
    static bool keys[SDL_SCANCODE_COUNT];
    const bool* state = SDL_GetKeyboardState(nullptr);
    for (int key = 0; key < SDL_SCANCODE_COUNT; key++)
    {
        keys[key] = state[key] || g_Latched[key];
        g_Latched[key] = false;
    }

    NativeUi::ReadBindings(keys, g_Gamepad, &bound);
    u32 buttons = bound.buttons;
    u8 pressure[PressureReportBytes] = {};
    pressure[PressureL2] = bound.l2Pressure;
    pressure[PressureR2] = bound.r2Pressure;
    u8 rightX = bound.rightX, rightY = bound.rightY, leftX = bound.leftX, leftY = bound.leftY;
    // Online co-op's tests: the left stick turned round on its own ($TWIN_COOP_TEST_RUN)
    Coop::TestStick(&leftX, &leftY);

    // The second player on this PC (coop/secondplayer.cpp) shares the game's own pad: its Start pauses in play; a pause is the
    // pauser's alone (only they move in its menu and resume); the menus and cutscenes take either player's buttons
    if (Coop::SecondPlayerWanted() && g_SecondOpen && g_SecondGamepad != nullptr)
    {
        NativeUi::BoundPad second;
        NativeUi::ReadGamepadOnly(g_SecondGamepad, &second);
        // $TWIN_P2_TEST_START="s1,s2,..." (tests): the second player's Start held for a moment at each of those seconds
        static const char* testStart = std::getenv("TWIN_P2_TEST_START");
        if (testStart != nullptr)
        {
            double seconds = static_cast<double>(SDL_GetTicks()) / 1000.0;
            for (const char* at = testStart; at != nullptr && *at != 0;)
            {
                double when = std::atof(at);
                if (seconds >= when && seconds < when + 0.2)
                {
                    second.buttons |= NativeUi::PadBitStart;
                }

                at = std::strchr(at, ',');
                at = at != nullptr ? at + 1 : nullptr;
            }
        }

        auto centred = [](u8 x, u8 y)
        {
            auto near = [](u8 value) { return value > 0x60 && value < 0xA0; };
            return near(x) && near(y);
        };

        static int pausedBy = 0;
        static bool firstStartWas = false;
        static bool secondStartWas = false;
        constexpr u32 PadBitStart = NativeUi::PadBitStart;
        bool firstStart = (buttons & PadBitStart) != 0;
        bool secondStart = (second.buttons & PadBitStart) != 0;
        bool firstPressed = firstStart && !firstStartWas;
        bool secondPressed = secondStart && !secondStartWas;
        firstStartWas = firstStart;
        secondStartWas = secondStart;
        int mode = Coop::SharedPadMode();
        static int modeWas = -1;
        if (mode != modeWas)
        {
            modeWas = mode;
            Native::Log("second player: the shared pad %s", mode == Coop::PadPlaying ? "in play (its Start pauses)"
                                                          : mode == Coop::PadPaused
                                                              ? (pausedBy == 2 ? "paused by player 2 (theirs alone)"
                                                                               : "paused by player 1 (theirs alone)")
                                                              : "in the menus (both)");
        }

        switch (mode)
        {
        case Coop::PadPlaying:
            // (Who pressed Start last in play: the pause that follows is theirs)
            if (firstPressed || secondPressed)
            {
                pausedBy = firstPressed ? 1 : 2;
            }

            buttons |= second.buttons & PadBitStart;
            break;
        case Coop::PadPaused:
            if (pausedBy == 2)
            {
                buttons = second.buttons;
                leftX = second.leftX;
                leftY = second.leftY;
                rightX = second.rightX;
                rightY = second.rightY;
                pressure[PressureL2] = second.l2Pressure;
                pressure[PressureR2] = second.r2Pressure;
            }

            break;
        default:
            pausedBy = 0;
            buttons |= second.buttons;
            if (centred(leftX, leftY))
            {
                leftX = second.leftX;
                leftY = second.leftY;
            }

            if (centred(rightX, rightY))
            {
                rightX = second.rightX;
                rightY = second.rightY;
            }

            break;
        }
    }

    // The digital buttons' pressures: fully pressed (the triggers have their own when a gamepad gives them)
    for (const PressureOf& of : Pressures)
    {
        if ((buttons & of.button) != 0 && pressure[of.byte] == 0)
        {
            pressure[of.byte] = Pressed;
        }
    }

    std::memset(data, 0, PressureReportBytes);
    data[0] = 0;
    data[ReportMode] = g_PressureMode ? ModePressure : ModeAnalog;
    data[ReportButtonsHigh] = static_cast<u8>(~buttons >> 8);
    data[ReportButtonsLow] = static_cast<u8>(~buttons);
    data[StickRightX] = rightX;
    data[StickRightY] = rightY;
    data[StickLeftX] = leftX;
    data[StickLeftY] = leftY;
    if (g_PressureMode)
    {
        for (u32 byte = PressureRight; byte <= PressureR2; byte++)
        {
            data[byte] = pressure[byte];
        }
    }

    return g_PressureMode ? PressureReportBytes : AnalogReportBytes;
}
