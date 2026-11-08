// The second player on this PC (coop.h): one world, two characters. The host's game makes a second character of the first player's
// object beside the first player, a character of its own (instancefactory.cpp's GameFactoryStandIn, while g_SpawningSecondPlayer:
// not the progress's, which stays the first player's), and gives it the second gamepad (the pads' port 1: platform/native/pads.cpp).
// A friend plays it from another PC through Parsec (their controller shows up here as a second gamepad).
//
// Asked for by coop.ini's players=2 or $TWIN_PLAYERS=2. Made again whenever play comes back (a level load, a checkpoint's restart).
#include "coop.h"

#include "native.h"
#include "ui/bindings.h"
#include "ui/nativeui.h"

#include "game/agents.h"
#include "game/chunkdata.h"
#include "game/chunkloading.h"
#include "game/collision.h"
#include "game/controllers.h"
#include "game/followcamera.h"
#include "game/gamecontroller.h"
#include "game/instancefactory.h"
#include "game/instances.h"
#include "game/pads.h"
#include "game/place.h"
#include "game/player.h"
#include "game/reference.h"
#include "game/sound.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace Coop
{
bool g_SpawningSecondPlayer = false;

namespace
{
// Where the second player comes in: beside the first (units along x)
constexpr float BesideFirst = 1.5f;
// Frames of play before it's made (the level's own spawns settled first)
constexpr int SettleFrames = 30;

struct Second
{
    InstanceContext* instance = nullptr;
    GameNode* characterNode = nullptr;
};
Second g_Second;

// Whether the second player is asked for: $TWIN_PLAYERS, else coop.ini's players
bool Wanted()
{
    static int wanted = -1;
    if (wanted < 0)
    {
        wanted = 0;
        const char* variable = std::getenv("TWIN_PLAYERS");
        if (variable != nullptr)
        {
            wanted = std::atoi(variable) >= 2 ? 1 : 0;
        }
        else if (FILE* file = std::fopen("coop.ini", "r"))
        {
            char line[256];
            while (std::fgets(line, sizeof(line), file) != nullptr)
            {
                if (std::strncmp(line, "players=", 8) == 0)
                {
                    wanted = std::atoi(line + 8) >= 2 ? 1 : 0;
                }
            }

            std::fclose(file);
        }

        Native::Log("second player: %s", wanted != 0 ? "on (the second gamepad plays it)" : "off");
    }

    return wanted != 0;
}

// Whether the second player is still the instance made for it (live, and its character node the one it was made with: a chunk let
// go, its memory may hold another instance)
bool Valid()
{
    return g_Second.instance != nullptr && InstanceLive(g_Second.instance) &&
           GetGameNode(&g_Second.instance->nodes, NodeCharacter) == g_Second.characterNode;
}

InstanceContext* FirstPlayer()
{
    return g_PlayerInstance != nullptr ? static_cast<InstanceContext*>(g_PlayerInstance->object) : nullptr;
}

CharacterAgent* AgentOf(InstanceContext* instance)
{
    auto* node = instance != nullptr ? static_cast<PlayerNode*>(GetGameNode(&instance->nodes, NodeCharacter)) : nullptr;
    return node != nullptr ? node->character : nullptr;
}

// A playable character's follow camera's instance (none without one)
InstanceContext* CameraOf(InstanceContext* character)
{
    auto* follow = character != nullptr ? static_cast<FollowNode*>(GetGameNode(&character->nodes, NodeFollow)) : nullptr;
    return follow != nullptr && follow->cameraInstance != nullptr ? static_cast<InstanceContext*>(follow->cameraInstance->object)
                                                                  : nullptr;
}

GamePad* SecondPad()
{
    auto* pads = static_cast<GamePadController*>(G_GamePadController);
    return pads != nullptr ? pads->pads[1] : nullptr;
}

// The second player made beside the first: the first's object, in the first's chunk, a character of its own with the second pad;
// its follow node out of its nodes for now (the first player's camera is the only one: a second view comes later)
void Make()
{
    InstanceContext* first = FirstPlayer();
    CharacterAgent* agent = AgentOf(first);
    if (first == nullptr || agent == nullptr || first->place == nullptr || first->chunk == nullptr)
    {
        return;
    }

    // Made in the chunk the first player is in now (its object's own chunk may be let go by then: the camera made in a chunk with no
    // data crashed, 2026-11-08, after a walk into another area of the beach)
    ChunkEntry* chunk = nullptr;
    auto* manager = static_cast<ChunkManager*>(g_ChunkManager);
    for (u32 i = 0; manager != nullptr && i < manager->count; i++)
    {
        ChunkEntry* entry = manager->entries[i];
        if (entry != nullptr && entry->data != nullptr && entry->data->chunk == first->chunk)
        {
            chunk = entry;
            break;
        }
    }

    if (chunk == nullptr)
    {
        ChunkEntry* own = ChunkOfIndex(g_ChunkManager, agent->chunkIndex);
        chunk = own != nullptr && own->data != nullptr && own->data->chunk != nullptr ? own : nullptr;
    }

    if (chunk == nullptr)
    {
        static int told = 0;
        if (told++ % 60 == 0)
        {
            Native::Log("second player: not made yet (the first player's chunk isn't loaded)");
        }

        return;
    }

    first->place->SyncPosition();
    Vector4 position = first->place->position;
    position.x += BesideFirst;
    s32 angles[3] = {0, 0, 0};
    InstanceFactory* factory = g_InstanceFactory;
    factory->SetInstanceProperties();
    factory->ClearUnused1();
    // No ID: an instance with one is released when it's put to sleep (a chunk change), the second player is to stay
    factory->ClearGivesIds();
    factory->ClearGivesFlagSlots();
    factory->creationFlags = 0;
    // Given the first player's properties (its state: visible, solid, awake: the object's own leave a playable character hidden
    // and out of the way until the layout's switch it on)
    g_SpawningSecondPlayer = true;
    InstanceContext* made = CreateInstanceFrom(factory, chunk, first, agent->objectId, &position, angles, 0);
    g_SpawningSecondPlayer = false;
    if (made == nullptr || made == first)
    {
        Native::Log("second player: not made (%s)", made == first ? "the first player stood in for it" : "nothing made");
        return;
    }

    static int madeCount = 0;
    madeCount++;
    Native::Log("second player: made (%d made so far this run)", madeCount);
    g_Second.instance = made;
    g_Second.characterNode = static_cast<GameNode*>(GetGameNode(&made->nodes, NodeCharacter));
    auto* controls = static_cast<ControlsNode*>(GetGameNode(&made->nodes, NodeControls));
    GamePad* pad = SecondPad();
    if (controls != nullptr)
    {
        controls->pad = pad != nullptr && pad->info != nullptr ? pad : nullptr;
    }

    // Its own camera, as the game shows the first player's (ShowCamera's follow camera): its rig on its lens, kept in its chunk
    auto* follow = static_cast<FollowNode*>(GetGameNode(&made->nodes, NodeFollow));
    InstanceContext* camera = CameraOf(made);
    if (follow != nullptr && camera != nullptr)
    {
        // As GameController::SwitchCharacter does for the character played: the rig's target the character, the follow node
        // stepped every frame, its camera awake
        AssignReference(&follow->camera.rig.ownTarget.followed, made);
        RegisterNode(follow->owner, 0, follow);
        CallVirtual<u32>(camera, camera->vtable, ReferencedObject::WakeSlot);
        ShowFollowCamera(&follow->camera, camera, made, 1);
        camera->flags.movesBetweenChunks = 0;
    }

    Native::Log("second player: made beside the first at (%.1f, %.1f, %.1f) in %s, %s", position.x, position.y, position.z,
                first->chunk->name.string != nullptr ? first->chunk->name.string : "?",
                controls == nullptr ? "with no controls' node" : (controls->pad != nullptr ? "the second gamepad plays it"
                                                                                           : "no second gamepad yet"));
}
}

InstanceContext* SecondPlayer()
{
    return Valid() ? g_Second.instance : nullptr;
}

bool SecondPlayerWanted()
{
    return Wanted();
}

// The two views: each frame drawn from one player's camera, the next from the other's (at 120 frames a second, each window shows 60:
// the game's own steps stay 60 a second, every other frame draws the same moment from the other camera)
namespace
{
// The view the next frame's drawing is (1 the second player's), the one the last drawing was, and the first player's camera while
// the second's is lent to the view for a drawing
int g_Drawing = 0;
int g_Drawn = 0;
bool g_Lent = false;
InstanceContext* g_FirstView = nullptr;

InstanceContext* ViewCamera(GameController* controller)
{
    Reference* shown = controller != nullptr ? controller->view.cameraObject : nullptr;
    return shown != nullptr ? static_cast<InstanceContext*>(shown->object) : nullptr;
}

// Whether there are two views now (the game shows its own rig most of the time in play, or the played character's follow camera:
// either is the first view)
bool TwoViewsOn(GameController* controller, InstanceContext* second, InstanceContext* secondCamera)
{
    return controller != nullptr && second != nullptr && secondCamera != nullptr && ViewCamera(controller) != nullptr &&
           controller->State() == GameController::StatePlaying &&
           (controller->states.camera == GameController::CameraFollow ||
            controller->states.camera == GameController::CameraGameRig);
}
}

namespace
{
bool g_TwoViews = false;
// The drawings counted, and what each camera's drawing last made of each instance it met (when, how far, in view)
u32 g_Drawings = 0;
struct Sighting
{
    u32 drawing[2] = {0, 0};
    u32 distance[2] = {0, 0};
    bool inView[2] = {false, false};
};
std::unordered_map<const InstanceContext*, Sighting> g_Seen;
// Drawings a camera's sighting counts for (each camera draws every other one)
constexpr u32 SeenDrawings = 4;

bool Recent(const Sighting& sighting, int view)
{
    return sighting.drawing[view] != 0 && g_Drawings - sighting.drawing[view] <= SeenDrawings;
}
}

bool TwoViews()
{
    return g_TwoViews;
}

void NoteSeen(InstanceContext* instance, unsigned distance, bool inView)
{
    int view = g_Lent ? 1 : 0;
    Sighting& sighting = g_Seen[instance];
    sighting.drawing[view] = g_Drawings;
    sighting.distance[view] = distance;
    sighting.inView[view] = inView;
    u32 nearest = distance;
    bool seen = inView;
    int other = 1 - view;
    if (Recent(sighting, other))
    {
        nearest = sighting.distance[other] < nearest ? sighting.distance[other] : nearest;
        seen = seen || sighting.inView[other];
    }

    instance->seen = nearest;
    instance->flags.inDrawnCell = seen ? 1 : 0;
}

bool SeenBySecondView(const InstanceContext* instance)
{
    if (!g_TwoViews)
    {
        return false;
    }

    auto found = g_Seen.find(instance);
    return found != g_Seen.end() && Recent(found->second, 1) && found->second.inView[1];
}

bool DrawingSecondView()
{
    return g_Lent;
}

// The third person camera (both players, F7 the game's own instead): the right stick (the mouse too for the first player) turns it
// round the character, it stays where it's left (nothing turns it on its own, nothing draws its eye), kept short of the walls. Its
// matrix written over the game's camera each frame before the drawing (the game's rig still runs, its place overwritten); on foot
// only (a vehicle's camera the game's own)
namespace
{
struct Orbit
{
    InstanceContext* camera = nullptr;
    float headingX = 0.0f;
    float headingZ = 1.0f;
    float pitch = 0.3f;
    float handed = 1.0f;
    u64 last = 0;
};
Orbit g_Orbits[2];
// (Farther and higher than the first try: the players felt they were under the character)
// (9 after the players asked for 20% more than 7.5)
constexpr float OrbitDistance = 9.0f;
// The mouse's turn at sensitivity 5: radians for each point it moves (about 0.17 degrees: a 360 in 2100 points)
constexpr float RadiansPerPoint = 0.0030f;
constexpr float TargetHeight = 1.7f;
constexpr float StartPitch = 0.42f;
constexpr float TurnSpeed = 3.4f;
constexpr float PitchSpeed = 2.2f;
constexpr float LowestPitch = -0.05f;
constexpr float HighestPitch = 1.15f;
// The surfaces that stop a camera (layout.h's SurfaceFlags::BlocksCamera)
constexpr u32 BlocksCamera = 0x20;

Vector4 CrossOf(const Vector4& a, const Vector4& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x, 0.0f};
}

float DotOf(const Vector4& a, const Vector4& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vector4 Unit(const Vector4& v)
{
    float length = std::sqrt(DotOf(v, v));
    return length > 1e-6f ? Vector4{v.x / length, v.y / length, v.z / length, 0.0f} : Vector4{0.0f, 0.0f, 1.0f, 0.0f};
}

float StickOf(GamePad* pad, bool vertical)
{
    if (pad == nullptr || pad->info == nullptr)
    {
        return 0.0f;
    }

    // (The report's byte: 128 the middle; the game's own dead zone on top)
    float value = (static_cast<float>(pad->info->report[vertical ? 5 : 4]) - 128.0f) / 127.0f;
    value = value < -1.0f ? -1.0f : (value > 1.0f ? 1.0f : value);
    return std::fabs(value) < 0.12f ? 0.0f : value;
}

void OrbitCamera(int player, InstanceContext* character, InstanceContext* camera, GamePad* pad)
{
    Orbit& orbit = g_Orbits[player];
    CharacterAgent* agent = AgentOf(character);
    if (character == nullptr || camera == nullptr || character->place == nullptr || camera->place == nullptr || agent == nullptr ||
        agent->vehicle != nullptr || character->chunk == nullptr)
    {
        orbit.camera = nullptr;
        return;
    }

    u64 now = SDL_GetTicksNS();
    float seconds = orbit.last != 0 ? static_cast<float>(now - orbit.last) / 1e9f : 0.0f;
    orbit.last = now;
    seconds = seconds > 0.1f ? 0.1f : seconds;
    character->place->SyncPosition();
    Vector4 target = character->place->position;
    target.y += TargetHeight;
    const Vector4 up = {0.0f, 1.0f, 0.0f, 0.0f};
    // Taken over from where the game's camera is (no jump), and which way its axes go (its x axis the screen's right)
    if (orbit.camera != camera)
    {
        RotateAndTranslate(camera->place);
        const Matrix4x4& m = camera->place->matrix;
        Vector4 side = {m.m[0][0], m.m[0][1], m.m[0][2], 0.0f};
        Vector4 cameraUp = {m.m[1][0], m.m[1][1], m.m[1][2], 0.0f};
        Vector4 forward = {m.m[2][0], m.m[2][1], m.m[2][2], 0.0f};
        orbit.handed = DotOf(CrossOf(cameraUp, forward), side) >= 0.0f ? 1.0f : -1.0f;
        Vector4 heading = Unit({forward.x, 0.0f, forward.z, 0.0f});
        orbit.headingX = heading.x;
        orbit.headingZ = heading.z;
        // (The heading the game's camera had; the height ours)
        orbit.pitch = StartPitch;
        orbit.camera = camera;
    }

    // The stick: right turns the view right (the heading toward the camera's right), up looks up. The stick's turn is a speed (a
    // push over time); the mouse's is the angle itself (below): a flick turns as far as it went, the slowest move as little
    float turn = StickOf(pad, false);
    float look = StickOf(pad, true);
    float angle = turn * TurnSpeed * seconds;
    float pitchBy = look * PitchSpeed * seconds;
    // The first player's mouse: the motion as it came (not the stick's conversion, which cut a fast move at the stick's end:
    // a flick hardly turned it), an angle for each point of it, the sensitivity option's (1 to 10, 5 the middle) scaling the angle.
    // The directions as the stick's conversion gave them (left turns left, up looks up; "invert" options kept). While the mouse
    // moves, the stick's report (the mouse in it too, in the game's way) isn't read
    if (player == 0)
    {
        float mouseX = 0.0f;
        float mouseY = 0.0f;
        NativeUi::TakeRawMouseLook(&mouseX, &mouseY);
        static u64 mouseAt = 0;
        if (mouseX != 0.0f || mouseY != 0.0f)
        {
            mouseAt = now;
            float perPoint = RadiansPerPoint * NativeUi::MouseSensitivity() / 5.0f;
            angle = mouseX * perPoint * (NativeUi::IsOn(NativeUi::OptionInvertCameraX) ? -1.0f : 1.0f);
            pitchBy = mouseY * perPoint * (NativeUi::IsOn(NativeUi::OptionInvertMouseY) ? -1.0f : 1.0f);
        }
        else if (now - mouseAt < 150000000ull)
        {
            angle = 0.0f;
            pitchBy = 0.0f;
        }
    }

    Vector4 heading = {orbit.headingX, 0.0f, orbit.headingZ, 0.0f};
    if (angle != 0.0f)
    {
        Vector4 right = Unit(CrossOf(up, heading));
        right = {right.x * orbit.handed, 0.0f, right.z * orbit.handed, 0.0f};
        heading = Unit({heading.x * std::cos(angle) + right.x * std::sin(angle), 0.0f,
                        heading.z * std::cos(angle) + right.z * std::sin(angle), 0.0f});
        orbit.headingX = heading.x;
        orbit.headingZ = heading.z;
    }

    // (The report's y: down is larger)
    orbit.pitch = std::fmax(LowestPitch, std::fmin(HighestPitch, orbit.pitch + pitchBy));
    Vector4 forward = Unit({heading.x * std::cos(orbit.pitch), -std::sin(orbit.pitch), heading.z * std::cos(orbit.pitch), 0.0f});
    Vector4 at = {target.x - forward.x * OrbitDistance, target.y - forward.y * OrbitDistance, target.z - forward.z * OrbitDistance,
                  1.0f};
    // Kept short of a wall between the character and the camera
    Vector4 hit;
    f32 distance = 0.0f;
    if (GetCollisionCheck(character->chunk, &target, &at, BlocksCamera, &distance, &hit, nullptr) != 0)
    {
        at = {hit.x + forward.x * 0.35f, hit.y + forward.y * 0.35f, hit.z + forward.z * 0.35f, 1.0f};
    }

    Vector4 side = Unit(CrossOf(up, forward));
    side = {side.x * orbit.handed, side.y * orbit.handed, side.z * orbit.handed, 0.0f};
    Vector4 cameraUp = Unit(CrossOf(forward, side));
    if (cameraUp.y < 0.0f)
    {
        cameraUp = {-cameraUp.x, -cameraUp.y, -cameraUp.z, 0.0f};
    }

    Matrix4x4 m;
    m.m[0][0] = side.x, m.m[0][1] = side.y, m.m[0][2] = side.z, m.m[0][3] = 0.0f;
    m.m[1][0] = cameraUp.x, m.m[1][1] = cameraUp.y, m.m[1][2] = cameraUp.z, m.m[1][3] = 0.0f;
    m.m[2][0] = forward.x, m.m[2][1] = forward.y, m.m[2][2] = forward.z, m.m[2][3] = 0.0f;
    m.m[3][0] = at.x, m.m[3][1] = at.y, m.m[3][2] = at.z, m.m[3][3] = 1.0f;
    SetPlaceMatrix(camera->place, &m);
}

void OrbitCameras()
{
    GameController* controller = G_GameController;
    bool playing = controller != nullptr && controller->State() == GameController::StatePlaying &&
                   (controller->states.camera == GameController::CameraFollow ||
                    controller->states.camera == GameController::CameraGameRig);
    if (!playing || !FreeCamera())
    {
        g_Orbits[0].camera = nullptr;
        g_Orbits[1].camera = nullptr;
        return;
    }

    auto* pads = static_cast<GamePadController*>(G_GamePadController);
    OrbitCamera(0, FirstPlayer(), ViewCamera(controller), pads != nullptr ? pads->pads[0] : nullptr);
    InstanceContext* second = SecondPlayer();
    if (second != nullptr)
    {
        OrbitCamera(1, second, CameraOf(second), pads != nullptr ? pads->pads[1] : nullptr);
    }
}
}

// The ping: where a player's camera looks, marked for both: the first player's middle mouse button, the second's right stick pressed
// (R3). A ring and a beam over the spot in each window (graphics/display.cpp draws it from PingMarker), the extra life's chime
namespace
{
struct Ping
{
    bool active = false;
    Vector4 at = {};
    u64 since = 0;
};
Ping g_Ping;
// Where the ping falls in each view's picture (-1 to 1 each way, the picture's middle 0), whether it's in front of the camera
struct PingScreen
{
    bool on = false;
    float x = 0.0f;
    float y = 0.0f;
    bool edge = false;
};
PingScreen g_PingScreen[2];
constexpr float PingSeconds = 7.0f;
constexpr float PingReach = 250.0f;
// The extra life's chime (oleg.cpp), a little louder than there
constexpr u32 PingSound = 0x10A;
constexpr float PingVolume = 0.35f;

void MakePing(int player)
{
    InstanceContext* camera = CameraOf(player == 0 ? FirstPlayer() : SecondPlayer());
    if (player == 0)
    {
        GameController* controller = G_GameController;
        InstanceContext* shown = ViewCamera(controller);
        camera = shown != nullptr ? shown : camera;
    }

    if (camera == nullptr || camera->place == nullptr || camera->chunk == nullptr)
    {
        return;
    }

    RotateAndTranslate(camera->place);
    const Matrix4x4& m = camera->place->matrix;
    Vector4 start = {m.m[3][0], m.m[3][1], m.m[3][2], 1.0f};
    Vector4 end = {start.x + m.m[2][0] * PingReach, start.y + m.m[2][1] * PingReach, start.z + m.m[2][2] * PingReach, 1.0f};
    Vector4 hit;
    f32 distance = 0.0f;
    constexpr u32 AnySolid = 0x10 | 0x20 | 0x40 | 0x80;
    if (GetCollisionCheck(camera->chunk, &start, &end, AnySolid, &distance, &hit, nullptr) == 0)
    {
        return;
    }

    g_Ping.active = true;
    g_Ping.at = {hit.x, hit.y, hit.z, 1.0f};
    g_Ping.since = SDL_GetTicksNS();
    PlaySoundById(PingVolume, OwnScale, PingSound, 0, 0, -1);
    Native::Log("second player: player %d pinged (%.1f, %.1f, %.1f)", player + 1, hit.x, hit.y, hit.z);
}

// Once a frame in play: the buttons that ping
void PingFrame()
{
    GameController* controller = G_GameController;
    if (controller == nullptr || controller->State() != GameController::StatePlaying)
    {
        return;
    }

    if (g_Ping.active && static_cast<float>(SDL_GetTicksNS() - g_Ping.since) / 1e9f > PingSeconds)
    {
        g_Ping.active = false;
    }

    static bool was[2] = {false, false};
    bool down[2] = {false, false};
    // The middle button counts while the window holds the mouse (play with mouse look)
    if (NativeUi::MouseLookActive())
    {
        down[0] = (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MMASK) != 0;
    }

    auto* pads = static_cast<GamePadController*>(G_GamePadController);
    GamePad* second = pads != nullptr ? pads->pads[1] : nullptr;
    if (second != nullptr && second->info != nullptr)
    {
        down[1] = (second->now & (1u << PadR3)) != 0;
    }

    // $TWIN_P2_TEST_PING="s1,s2": the first player pings at those seconds (tests)
    static const char* test = std::getenv("TWIN_P2_TEST_PING");
    if (test != nullptr)
    {
        double seconds = static_cast<double>(SDL_GetTicks()) / 1000.0;
        for (const char* at = test; at != nullptr && *at != 0;)
        {
            double when = std::atof(at);
            if (seconds >= when && seconds < when + 0.2)
            {
                down[0] = true;
            }

            at = std::strchr(at, ',');
            at = at != nullptr ? at + 1 : nullptr;
        }
    }

    for (int player = 0; player < 2; player++)
    {
        if (down[player] && !was[player] && (player == 0 ? FirstPlayer() : SecondPlayer()) != nullptr)
        {
            MakePing(player);
        }

        was[player] = down[player];
    }
}

// After a view's drawing: the ping's place in that view's picture, from the matrices the drawing made
void ProjectPing(int view)
{
    PingScreen& out = g_PingScreen[view];
    out.on = false;
    GameController* controller = G_GameController;
    if (!g_Ping.active || controller == nullptr)
    {
        return;
    }

    const Matrix4x4& toCamera = controller->view.toCamera;
    const Matrix4x4& projection = controller->view.projection;
    const Vector4& p = g_Ping.at;
    float cx = p.x * toCamera.m[0][0] + p.y * toCamera.m[1][0] + p.z * toCamera.m[2][0] + toCamera.m[3][0];
    float cy = p.x * toCamera.m[0][1] + p.y * toCamera.m[1][1] + p.z * toCamera.m[2][1] + toCamera.m[3][1];
    float cz = p.x * toCamera.m[0][2] + p.y * toCamera.m[1][2] + p.z * toCamera.m[2][2] + toCamera.m[3][2];
    if (cz < 0.2f)
    {
        // Behind the camera: shown at the edge on its side, so the player turns to it
        out.on = true;
        out.edge = true;
        out.x = cx >= 0.0f ? 0.92f : -0.92f;
        out.y = 0.0f;
        return;
    }

    float x = cx * projection.m[0][0] / cz;
    float y = cy * projection.m[1][1] / cz;
    out.edge = std::fabs(x) > 0.92f || std::fabs(y) > 0.92f;
    out.x = std::fmax(-0.92f, std::fmin(0.92f, x));
    out.y = std::fmax(-0.92f, std::fmin(0.92f, y));
    out.on = true;
    // $TWIN_P2_LOG (tests): where the ping falls, twice a second
    static const bool log = std::getenv("TWIN_P2_LOG") != nullptr;
    static u32 count = 0;
    if (log && view == 0 && ++count % 30 == 0)
    {
        Native::Log("second player: the ping in view 0 at (%.2f, %.2f), camera depth %.1f", x, y, cz);
    }
}
}

bool PingMarker(int view, float* x, float* y, float* strength, bool* edge)
{
    if (!g_Ping.active || view < 0 || view > 1 || !g_PingScreen[view].on)
    {
        return false;
    }

    float age = static_cast<float>(SDL_GetTicksNS() - g_Ping.since) / 1e9f;
    if (age > PingSeconds)
    {
        return false;
    }

    *x = g_PingScreen[view].x;
    *y = g_PingScreen[view].y;
    *edge = g_PingScreen[view].edge;
    // Strong at first, fading out through the last second
    *strength = age > PingSeconds - 1.0f ? (PingSeconds - age) : 1.0f;
    return true;
}

// The third person camera (on; $TWIN_ORBIT_CAMERA=0 the game's own, as F7 does)
bool OrbitCameraWanted()
{
    static const char* setting = std::getenv("TWIN_ORBIT_CAMERA");
    return setting == nullptr || std::atoi(setting) != 0;
}

void BeforeDraw()
{
    if (Wanted() && OrbitCameraWanted())
    {
        OrbitCameras();
    }

    g_Drawings++;
    // (Forgotten now and then: instances let go leave their entries, the memory taken by others)
    if (!g_TwoViews)
    {
        g_Seen.clear();
    }
    else if (g_Drawings % 600 == 0)
    {
        for (auto entry = g_Seen.begin(); entry != g_Seen.end();)
        {
            entry = Recent(entry->second, 0) || Recent(entry->second, 1) ? std::next(entry) : g_Seen.erase(entry);
        }
    }

    g_Drawn = 0;
    g_Lent = false;
    if (g_Drawing != 1)
    {
        return;
    }

    GameController* controller = G_GameController;
    InstanceContext* second = SecondPlayer();
    InstanceContext* secondCamera = CameraOf(second);
    if (!TwoViewsOn(controller, second, secondCamera))
    {
        return;
    }

    g_FirstView = ViewCamera(controller);
    AssignReference(&controller->view.cameraObject, secondCamera);
    g_Lent = true;
    g_Drawn = 1;
}

void AfterDraw()
{
    // The ping's place in the picture just drawn (the first player's view 0, the second's 1)
    ProjectPing(g_Lent ? 1 : 0);
    if (!g_Lent)
    {
        return;
    }

    g_Lent = false;
    GameController* controller = G_GameController;
    InstanceContext* back = g_FirstView != nullptr && InstanceLive(g_FirstView) ? g_FirstView : CameraOf(FirstPlayer());
    if (controller != nullptr && back != nullptr)
    {
        AssignReference(&controller->view.cameraObject, back);
    }
}

const Matrix4x4* MoveView(InstanceContext* character)
{
    InstanceContext* second = SecondPlayer();
    if (second == nullptr || character == nullptr)
    {
        return nullptr;
    }

    // (Out of the drawing the view's camera is always the first player's)
    InstanceContext* camera = character == second ? CameraOf(second) : ViewCamera(G_GameController);
    if (camera == nullptr || camera->place == nullptr || g_Lent)
    {
        return nullptr;
    }

    RotateAndTranslate(camera->place);
    return &camera->place->matrix;
}

int TwoViewsFrame()
{
    GameController* controller = G_GameController;
    InstanceContext* second = SecondPlayer();
    InstanceContext* secondCamera = CameraOf(second);
    bool on = TwoViewsOn(controller, second, secondCamera);
    g_TwoViews = on;
    // Why there's one view while there's a second player (once every 3 s)
    static u32 calls = 0;
    calls++;
    if (!on && second != nullptr && calls % 360 == 1)
    {
        Native::Log("second player: one view (view camera %s, second camera %s, state %u, camera kind %u)",
                    ViewCamera(controller) != nullptr ? "there" : "none", secondCamera != nullptr ? "there" : "none",
                    controller != nullptr ? controller->State() : 99u,
                    controller != nullptr ? static_cast<unsigned>(controller->states.camera) : 99u);
    }

    int drawn = g_Drawn;
    int next = on ? 1 - g_Drawing : 0;

    // Two views share the frames (each window gets every other one): a rate under 120 is raised to 120 (each window 60), a rate
    // the player chose at 120 or above (144, unlimited) is left alone. It was set to 120 whatever the choice, and saved as it:
    // the player's unlimited became 120, each window 60 (the host's 200 Hz screen no longer smooth, 2026-11-08)
    static bool rateSet = false;
    if (on && !rateSet)
    {
        rateSet = true;
        constexpr s32 Rate120 = 2;
        if (NativeUi::GetOption(NativeUi::OptionRefresh60) < Rate120)
        {
            NativeUi::SetOption(NativeUi::OptionRefresh60, Rate120);
            Native::Log("second player: two views, the frame rate raised to 120 (each window 60)");
        }
    }

    g_Drawing = next;
    // One view (a cutscene, a pause, the menus): both windows show it
    if (!on && drawn == 0 && Wanted())
    {
        return BothViews;
    }

    return drawn;
}

// The backpack (one world, two characters): either player rides in the other's backpack, its character hidden (ModelDrawn,
// HidesShadow), out of the others' way (collision off), its pad taken (kept to give back), its hits and pickups off, carried where the
// other is every frame (its camera follows it there). Select on the player's gamepad (F5 on the keyboard for the first player) in and
// out; the carrier's Select lets the rider out
namespace
{
struct Ride
{
    int rider = -1;
    InstanceContext* instance = nullptr;
    GamePad* pad = nullptr;
    u32 collision = 0;
};
Ride g_Ride;

InstanceContext* PlayerOf(int player)
{
    return player == 0 ? FirstPlayer() : SecondPlayer();
}

ControlsNode* ControlsOf(InstanceContext* instance)
{
    return instance != nullptr ? static_cast<ControlsNode*>(GetGameNode(&instance->nodes, NodeControls)) : nullptr;
}

void PutAt(InstanceContext* instance, const Vector4& to)
{
    instance->place->SyncPosition();
    Vector4 at = to;
    at.w = 1.0f;
    if (instance->place->MoveTo(&at))
    {
        QueueObject(instance);
    }
}

void StartRide(int rider)
{
    InstanceContext* instance = PlayerOf(rider);
    InstanceContext* carrier = PlayerOf(1 - rider);
    ControlsNode* controls = ControlsOf(instance);
    CharacterAgent* agent = AgentOf(instance);
    if (instance == nullptr || carrier == nullptr || controls == nullptr || agent == nullptr)
    {
        return;
    }

    g_Ride.rider = rider;
    g_Ride.instance = instance;
    g_Ride.pad = controls->pad;
    g_Ride.collision = instance->flags.collisionActive;
    controls->pad = nullptr;
    instance->flags.collisionActive = 0;
    agent->moveInput = {0.0f, 0.0f, 0.0f, 1.0f};
    agent->eventFlags.eventsOff = 1;
    Native::Log("second player: player %d into player %d's backpack", rider + 1, 2 - rider);
}

void EndRide()
{
    if (g_Ride.rider < 0)
    {
        return;
    }

    InstanceContext* instance = g_Ride.instance;
    if (instance != nullptr && InstanceLive(instance) && instance == PlayerOf(g_Ride.rider))
    {
        ControlsNode* controls = ControlsOf(instance);
        CharacterAgent* agent = AgentOf(instance);
        if (controls != nullptr && controls->pad == nullptr)
        {
            controls->pad = g_Ride.pad;
        }

        if (agent != nullptr)
        {
            agent->eventFlags.eventsOff = 0;
        }

        instance->flags.collisionActive = g_Ride.collision;
        // Out beside the carrier (a little above: never under the ground)
        InstanceContext* carrier = PlayerOf(1 - g_Ride.rider);
        if (carrier != nullptr && carrier->place != nullptr)
        {
            carrier->place->SyncPosition();
            Vector4 to = carrier->place->position;
            // (On the carrier's own spot, a little up: a step aside could be in a wall or over a drop)
            to.y += 0.3f;
            PutAt(instance, to);
        }
    }

    Native::Log("second player: player %d out of the backpack", g_Ride.rider + 1);
    g_Ride = Ride();
}

// Select pressed this frame on a player's pad (F5 too for the first player)
bool BackpackPressed(int player)
{
    static bool was[2] = {false, false};
    auto* pads = static_cast<GamePadController*>(G_GamePadController);
    GamePad* pad = pads != nullptr ? pads->pads[player] : nullptr;
    bool down = pad != nullptr && pad->info != nullptr && (pad->now & (1u << PadSelect)) != 0;
    if (player == 0)
    {
        const bool* keys = SDL_GetKeyboardState(nullptr);
        down = down || (keys != nullptr && keys[SDL_SCANCODE_F5]);
    }

    bool pressed = down && !was[player];
    was[player] = down;
    return pressed;
}

// The leash: the world is loaded around the first player only, so the two can't go far apart. Past WarnAt the windows' edges turn
// red (LeashWarning: stronger nearer the limit), at LimitAt the second player is taken into the first's backpack
// (20 and 30 were too short: the players were pulled together all the time)
// (Four times the 55 and 75 before, as the players asked)
constexpr float WarnAt = 220.0f;
constexpr float LimitAt = 300.0f;
float g_Leash = 0.0f;

void LeashFrame()
{
    g_Leash = 0.0f;
    InstanceContext* first = FirstPlayer();
    InstanceContext* second = SecondPlayer();
    if (first == nullptr || second == nullptr || first->place == nullptr || second->place == nullptr)
    {
        return;
    }

    first->place->SyncPosition();
    second->place->SyncPosition();
    float dx = first->place->position.x - second->place->position.x;
    float dy = first->place->position.y - second->place->position.y;
    float dz = first->place->position.z - second->place->position.z;
    float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance >= LimitAt)
    {
        Native::Log("second player: too far from the first (%.1f), into the first player's backpack", distance);
        StartRide(1);
        return;
    }

    g_Leash = distance <= WarnAt ? 0.0f : (distance - WarnAt) / (LimitAt - WarnAt);
}

void BackpackFrame()
{
    g_Leash = 0.0f;
    bool pressed[2] = {BackpackPressed(0), BackpackPressed(1)};
    // $TWIN_P2_TEST_BACKPACK="in,out" (tests): the second player into the first's backpack that many seconds in, out at the second
    static const char* test = std::getenv("TWIN_P2_TEST_BACKPACK");
    if (test != nullptr)
    {
        static int stage = 0;
        double seconds = static_cast<double>(SDL_GetTicks()) / 1000.0;
        const char* comma = std::strchr(test, ',');
        if ((stage == 0 && seconds >= std::atof(test)) || (stage == 1 && comma != nullptr && seconds >= std::atof(comma + 1)))
        {
            stage++;
            pressed[1] = true;
        }
    }

    if (g_Ride.rider >= 0)
    {
        // The rider or the carrier lets the rider out; gone (made again), it's over
        if (pressed[0] || pressed[1] || g_Ride.instance != PlayerOf(g_Ride.rider) || PlayerOf(1 - g_Ride.rider) == nullptr)
        {
            EndRide();
            return;
        }

        InstanceContext* carrier = PlayerOf(1 - g_Ride.rider);
        carrier->place->SyncPosition();
        PutAt(g_Ride.instance, carrier->place->position);
        if (CharacterAgent* agent = AgentOf(g_Ride.instance))
        {
            agent->moveInput = {0.0f, 0.0f, 0.0f, 1.0f};
        }

        return;
    }

    for (int player = 0; player < 2; player++)
    {
        if (pressed[player])
        {
            StartRide(player);
            return;
        }
    }

    LeashFrame();
}

ModelNode* ModelOf(InstanceContext* instance)
{
    return instance != nullptr ? static_cast<ModelNode*>(GetGameNode(&instance->nodes, NodeModel)) : nullptr;
}
}

namespace
{
// A cutscene playing: the second player isn't in it (hidden, put beside the first once it's over)
bool InCutscene()
{
    GameController* controller = G_GameController;
    return controller != nullptr && controller->State() == GameController::StateWatching;
}
}

namespace
{
// The second player's model, kept each frame (looked up once, not at every model's drawing)
const ModelNode* g_SecondModel = nullptr;
}

InstanceContext* NearerPlayer(InstanceContext* from, InstanceContext* played)
{
    InstanceContext* second = g_SecondModel != nullptr ? SecondPlayer() : nullptr;
    if (from == nullptr || played == nullptr || second == nullptr || g_Ride.rider >= 0 || from->place == nullptr ||
        played->place == nullptr || second->place == nullptr || from->chunk == nullptr)
    {
        return played;
    }

    auto squared = [](ObjectPlace* a, ObjectPlace* b)
    {
        a->SyncPosition();
        b->SyncPosition();
        float dx = a->position.x - b->position.x;
        float dy = a->position.y - b->position.y;
        float dz = a->position.z - b->position.z;
        return dx * dx + dy * dy + dz * dz;
    };

    return squared(from->place, second->place) < squared(from->place, played->place) ? second : played;
}

InstanceContext* PlayerFor(InstanceContext* asking, InstanceContext* player)
{
    // (Anything asking, not only Cortex and Nina: a worm the second player jumped on launched the first, again and again)
    if (g_SecondModel == nullptr || asking == nullptr || player == nullptr || asking == player)
    {
        return player;
    }

    InstanceContext* second = SecondPlayer();
    return asking == second ? second : NearerPlayer(asking, player);
}

bool TintsGreen(const ModelNode* node)
{
    return node != nullptr && node == g_SecondModel;
}

void Greened(float* red, float* green, float* blue)
{
    float bright = (*red + *green + *blue) / 3.0f;
    *red = *red * 0.25f;
    *blue = *blue * 0.3f;
    *green = *green * 0.6f + bright * 0.7f;
}

float LeashWarning()
{
    GameController* controller = G_GameController;
    return controller != nullptr && controller->State() == GameController::StatePlaying ? g_Leash : 0.0f;
}

int SharedPadMode()
{
    GameController* controller = G_GameController;
    u32 state = controller != nullptr ? controller->State() : GameController::NoState;
    if (state == GameController::StatePlaying)
    {
        return PadPlaying;
    }

    return state == GameController::StatePaused ? PadPaused : PadMenus;
}

bool RidingHides(const ModelNode* node)
{
    if (node == nullptr)
    {
        return false;
    }

    if (g_Ride.rider >= 0 && node == ModelOf(g_Ride.instance))
    {
        return true;
    }

    if (!InCutscene())
    {
        return false;
    }

    InstanceContext* second = SecondPlayer();
    return second != nullptr && node == ModelOf(second);
}

bool RidingHidesShadow(const InstanceContext* instance)
{
    if (instance == nullptr)
    {
        return false;
    }

    if (g_Ride.rider >= 0 && instance == g_Ride.instance)
    {
        return true;
    }

    return InCutscene() && instance == SecondPlayer();
}

void SecondPlayerFrame()
{
    if (!Wanted())
    {
        return;
    }

    g_SecondModel = ModelOf(SecondPlayer());
    PingFrame();

    GameController* controller = G_GameController;
    u32 state = controller != nullptr ? controller->State() : GameController::NoState;
    static int playingFrames = 0;
    // Out of play since it was there (a death's restart, a level's load): put beside the first player again once play is back, the
    // same character (made again only when it's gone: a second made each time stayed, a crowd of them after a few deaths)
    static bool returning = false;
    if (state != GameController::StatePlaying)
    {
        // A cutscene: the second player comes back beside the first once it's over (the cutscene may have moved the first)
        if (state == GameController::StateWatching && Valid())
        {
            EndRide();
            returning = true;
            playingFrames = 0;
        }
        else if (state != GameController::StateWatching && state != GameController::StatePaused &&
                 state != GameController::StateWaiting)
        {
            returning = true;
            playingFrames = 0;
            // (A death's restart, a level's load: both come back on foot)
            EndRide();
        }

        return;
    }

    if (returning && Valid())
    {
        // Back in play (a death's restart, a cutscene's end): into the first player's backpack, carried from the first frame (put
        // beside the first it sometimes landed in a wall or under the ground, or stayed where it died, dying again); Select gets out
        returning = false;
        playingFrames = 0;
        if (g_Ride.rider < 0)
        {
            StartRide(1);
            Native::Log("second player: back in play, in the first player's backpack");
        }
    }
    else if (returning)
    {
        // Gone while out of play (its chunk let go): made again below
        returning = false;
        playingFrames = 0;
    }

    if (Valid())
    {
        // $TWIN_P2_LOG (tests): where it is and what it is, every 2 seconds
        static const bool log = std::getenv("TWIN_P2_LOG") != nullptr;
        static int logFrames = 0;
        if (log && ++logFrames >= 120)
        {
            logFrames = 0;
            InstanceContext* second = g_Second.instance;
            second->place->SyncPosition();
            CharacterAgent* agent = AgentOf(second);
            auto* controls = static_cast<ControlsNode*>(GetGameNode(&second->nodes, NodeControls));
            GamePad* pad = controls != nullptr ? controls->pad : nullptr;
            Native::Log("second player: at (%.1f, %.1f, %.1f), asleep %u, visible %u, drawn cell %u, collision %u, pad %s (left x %d), "
                        "move input (%.2f, %.2f), chunk %s",
                        second->place->position.x, second->place->position.y, second->place->position.z,
                        static_cast<unsigned>(second->flags.asleep), static_cast<unsigned>(second->flags.visible),
                        static_cast<unsigned>(second->flags.inDrawnCell), static_cast<unsigned>(second->flags.collisionActive),
                        pad != nullptr ? "given" : "none", pad != nullptr && pad->info != nullptr ? pad->info->leftX : -999,
                        agent != nullptr ? agent->moveInput.x : 0.0f, agent != nullptr ? agent->moveInput.z : 0.0f,
                        second->chunk != nullptr && second->chunk->name.string != nullptr ? second->chunk->name.string : "none");
        }

        // The pad given again when it wasn't there yet (a gamepad plugged in later)
        auto* controls = static_cast<ControlsNode*>(GetGameNode(&g_Second.instance->nodes, NodeControls));
        GamePad* pad = SecondPad();
        if (controls != nullptr && controls->pad == nullptr && pad != nullptr && pad->info != nullptr && g_Ride.rider != 1)
        {
            controls->pad = pad;
        }

        // The first player switched to another character (Cortex, Nina, back to Crash): the second becomes the same character (the
        // old one put to sleep: out of the world's steps and drawing, its memory the game's to let go with its chunk)
        CharacterAgent* firstAgent = AgentOf(FirstPlayer());
        CharacterAgent* secondAgent = AgentOf(g_Second.instance);
        if (firstAgent != nullptr && secondAgent != nullptr && firstAgent->objectId != secondAgent->objectId)
        {
            Native::Log("second player: the first player is another character now (object %u, was %u): the second follows",
                        static_cast<unsigned>(firstAgent->objectId), static_cast<unsigned>(secondAgent->objectId));
            EndRide();
            InstanceContext* old = g_Second.instance;
            old->flags.visible = 0;
            old->flags.collisionActive = 0;
            if (ControlsNode* oldControls = ControlsOf(old))
            {
                oldControls->pad = nullptr;
            }

            CallVirtual<u32>(old, old->vtable, ReferencedObject::SleepSlot);
            g_Second = Second();
            g_SecondModel = nullptr;
            playingFrames = SettleFrames - 1;
            return;
        }

        BackpackFrame();
        return;
    }

    g_Second = Second();
    if (++playingFrames >= SettleFrames)
    {
        playingFrames = 0;
        Make();
        // Made: in the first player's backpack to start with (never in a wall); Select gets out
        if (Valid() && g_Ride.rider < 0)
        {
            StartRide(1);
        }
    }
}
}
