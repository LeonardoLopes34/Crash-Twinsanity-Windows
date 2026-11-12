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
#include "game/scenery.h"
#include "game/sound.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <vector>
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
    if (Active())
    {
        return false;
    }

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
    InstanceContext* local = LocalPlayer();
    if (local != nullptr)
    {
        return local;
    }

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
    if (Valid())
    {
        return g_Second.instance;
    }

    return Active() ? NetworkPartner() : nullptr;
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
// When the next frame starts (TwoViewsFrame's choice; 0: as the frame rate paces it)
u64 g_PaceAt = 0;
// The last three choices' turns (a picture is shown two frames after its choice) and how many there have been since two views began
u64 g_Turns[3] = {0, 0, 0};
u64 g_TurnCount = 0;
// A frame's start and its work (nanoseconds, a running mean); whether the first window is at the second's rate for now, and since
// when the work has fitted with room
u64 g_FrameStart = 0;
u64 g_FrameWork = 0;
double g_FrameCost = 0.0;
bool g_Reduced = false;
u64 g_FitsSince = 0;

// A window's frames a second: the first's 90 (half the 180 Hz screen's refreshes), the second's 60 (Parsec's stream), or
// $TWIN_P1_FPS and $TWIN_P2_FPS
int ViewRate(int view)
{
    static int rates[2] = {0, 0};
    if (rates[view] == 0)
    {
        const char* variable = std::getenv(view == 0 ? "TWIN_P1_FPS" : "TWIN_P2_FPS");
        int rate = variable != nullptr ? std::atoi(variable) : 0;
        // (Up to 1000: "as fast as it goes", to measure what the PC gives, DOIS JOGADORES SEM TRAVA.bat)
        rates[view] = rate >= 20 && rate <= 1000 ? rate : (view == 0 ? 90 : 60);
        Native::Log("two players: window %d at %d frames a second", view + 1, rates[view]);
    }

    return rates[view];
}
bool g_Lent = false;
InstanceContext* g_FirstView = nullptr;

InstanceContext* ViewCamera(GameController* controller)
{
    Reference* shown = controller != nullptr ? controller->view.cameraObject : nullptr;
    return shown != nullptr ? static_cast<InstanceContext*>(shown->object) : nullptr;
}

// The first player paired with Cortex on a vehicle (the Rollerbrawl, the ball in the caves; the Humiliskate): nothing for the
// second player to do there, it rides in the first's backpack and both windows show the first's view
bool OnVehicle(GameController* controller)
{
    u32 pairing = controller != nullptr ? controller->progress.play.pairing : PairingAlone;
    return pairing == PairingRollerbrawl || pairing == PairingHumiliskate;
}

// Whether there are two views now (the game shows its own rig most of the time in play, or the played character's follow camera:
// either is the first view)
bool TwoViewsOn(GameController* controller, InstanceContext* second, InstanceContext* secondCamera)
{
    return Wanted() && controller != nullptr && !OnVehicle(controller) && second != nullptr && secondCamera != nullptr &&
           ViewCamera(controller) != nullptr &&
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

bool LastDrawnSecondView()
{
    return g_Drawn == 1;
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

}

// Who rides in the other's backpack now (0 the first player, 1 the second, -1 nobody): defined with the backpack below
int RiderPlayer();

namespace
{
void OrbitCamera(int player, InstanceContext* character, InstanceContext* camera, GamePad* pad)
{
    Orbit& orbit = g_Orbits[player];
    // The rider's view is the carrier's (copied by OrbitCameras): its own stick and mouse turn nothing
    if (RiderPlayer() == player)
    {
        orbit.camera = nullptr;
        return;
    }

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
        Native::Log("camera: the third person camera for player %d%s", player + 1, Active() ? " (online co-op)" : "");
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
    if (Wanted() && second != nullptr)
    {
        OrbitCamera(1, second, CameraOf(second), pads != nullptr ? pads->pads[1] : nullptr);
    }

    // The rider sees what the carrier sees: its camera put where the carrier's is, each frame
    int rider = RiderPlayer();
    if (rider >= 0 && second != nullptr)
    {
        InstanceContext* riderCamera = rider == 0 ? ViewCamera(controller) : CameraOf(second);
        InstanceContext* carrierCamera = rider == 0 ? CameraOf(second) : ViewCamera(controller);
        if (riderCamera != nullptr && carrierCamera != nullptr && riderCamera != carrierCamera && riderCamera->place != nullptr &&
            carrierCamera->place != nullptr)
        {
            RotateAndTranslate(carrierCamera->place);
            Matrix4x4 same = carrierCamera->place->matrix;
            SetPlaceMatrix(riderCamera->place, &same);
        }
    }
}
}

// The minimap: a circle in a corner of each window, centred on the player of that window and turned with its camera (up is where
// the camera looks), with what's near: the other player, enemies, crates, wumpa and the other characters, and the ping. The things
// listed in the world a few times a second (BeforeDraw), put on each window's circle after its picture's drawing (AfterDraw: the
// camera matrices of that drawing). M turns the first player's on and off, the D-pad's up the second's
namespace
{
constexpr int MaxMiniDots = 96;
}

// The ping's place on the ground, when there is one (defined with the ping below)
bool PingPlace(float* x, float* z);

namespace
{
// What a dot is: drawn in its colour (display.cpp)
enum MiniKind : int
{
    MiniEnemy = 1,
    MiniCrate = 2,
    MiniPickup = 3,
    MiniCharacter = 4,
    MiniOther = 5,
    MiniPing = 6,
};
// How far the circle's edge is, in world units
constexpr float MiniRange = 60.0f;

struct MiniThing
{
    float x, y, z;
    int kind;
};
std::vector<MiniThing> g_MiniThings;
// Each window's circle: dots (x, y in -1 to 1, kind, unused), how many, whether it shows
struct Minimap
{
    float dots[MaxMiniDots * 4];
    int count = 0;
    bool valid = false;
    float facing = 0.0f;
};
Minimap g_Minimap[2];
// Off to start with (2026-11-12, the players' ask, to see the frames without it: its list goes through every live instance a few
// times a second, and with the world around both players there are more): M or the D-pad's up shows it
bool g_MiniWanted[2] = {false, false};

struct MiniScan
{
    InstanceContext* first;
    InstanceContext* second;
    ChunkData* chunk;
    float cx, cz;
};

void ScanMini(InstanceContext* instance, void* user)
{
    auto* scan = static_cast<MiniScan*>(user);
    if (instance == scan->first || instance == scan->second || instance->chunk != scan->chunk || instance->place == nullptr ||
        instance->flags.asleep || !instance->flags.visible)
    {
        return;
    }

    u32 mask = instance->nodes.mask;
    int kind = 0;
    if ((mask & (1u << NodeCreature)) != 0)
    {
        kind = MiniEnemy;
    }
    else if ((mask & (1u << NodeCharacter)) != 0)
    {
        kind = MiniCharacter;
    }
    else if ((mask & (1u << NodeCrate)) != 0)
    {
        kind = MiniCrate;
    }
    else if ((mask & (1u << NodePickup)) != 0)
    {
        kind = MiniPickup;
    }

    if (kind == 0)
    {
        return;
    }

    instance->place->SyncPosition();
    const Vector4& p = instance->place->position;
    float dx = p.x - scan->cx;
    float dz = p.z - scan->cz;
    if (dx * dx + dz * dz > MiniRange * MiniRange * 1.2f)
    {
        return;
    }

    g_MiniThings.push_back({p.x, p.y, p.z, kind});
}

// A few times a second: what's near the two players
void ListMiniThings()
{
    g_MiniThings.clear();
    InstanceContext* first = FirstPlayer();
    InstanceContext* second = SecondPlayer();
    if (first == nullptr || first->place == nullptr || first->chunk == nullptr)
    {
        return;
    }

    first->place->SyncPosition();
    MiniScan scan = {first, second, first->chunk, first->place->position.x, first->place->position.z};
    ForEachLiveInstance(ScanMini, &scan);
    // The nearest to either player first (a crowded place fills the circle's dots from them)
    float sx = scan.cx;
    float sz = scan.cz;
    float ox = sx;
    float oz = sz;
    if (second != nullptr && second->place != nullptr)
    {
        second->place->SyncPosition();
        ox = second->place->position.x;
        oz = second->place->position.z;
    }

    auto nearness = [&](const MiniThing& t)
    {
        float a = (t.x - sx) * (t.x - sx) + (t.z - sz) * (t.z - sz);
        float b = (t.x - ox) * (t.x - ox) + (t.z - oz) * (t.z - oz);
        return a < b ? a : b;
    };
    std::sort(g_MiniThings.begin(), g_MiniThings.end(),
              [&](const MiniThing& a, const MiniThing& b) { return nearness(a) < nearness(b); });
    if (g_MiniThings.size() > 400)
    {
        g_MiniThings.resize(400);
    }
}

// After a view's drawing: the circle's dots for that view
void ProjectMinimap(int view)
{
    Minimap& map = g_Minimap[view];
    map.valid = false;
    map.count = 0;
    GameController* controller = G_GameController;
    InstanceContext* centre = view == 0 ? FirstPlayer() : SecondPlayer();
    InstanceContext* other = view == 0 ? SecondPlayer() : FirstPlayer();
    if (controller == nullptr || centre == nullptr || centre->place == nullptr || !g_MiniWanted[view] ||
        controller->State() != GameController::StatePlaying)
    {
        return;
    }

    // The camera's right and forward on the ground (the columns of the world-to-camera matrix)
    const Matrix4x4& toCamera = controller->view.toCamera;
    float rightX = toCamera.m[0][0];
    float rightZ = toCamera.m[2][0];
    float forwardX = toCamera.m[0][2];
    float forwardZ = toCamera.m[2][2];
    float forwardLength = std::sqrt(forwardX * forwardX + forwardZ * forwardZ);
    if (forwardLength < 0.05f)
    {
        // Looking straight up or down: the heading kept from the right axis
        forwardX = rightZ;
        forwardZ = -rightX;
        forwardLength = std::sqrt(forwardX * forwardX + forwardZ * forwardZ);
        if (forwardLength < 0.05f)
        {
            return;
        }
    }

    forwardX /= forwardLength;
    forwardZ /= forwardLength;
    float rightLength = std::sqrt(rightX * rightX + rightZ * rightZ);
    rightX = rightLength > 0.05f ? rightX / rightLength : forwardZ;
    rightZ = rightLength > 0.05f ? rightZ / rightLength : -forwardX;
    centre->place->SyncPosition();
    const Vector4 c = centre->place->position;
    auto put = [&](float x, float z, int kind, bool clamp)
    {
        if (map.count >= MaxMiniDots)
        {
            return;
        }

        float dx = x - c.x;
        float dz = z - c.z;
        float mx = (dx * rightX + dz * rightZ) / MiniRange;
        float my = (dx * forwardX + dz * forwardZ) / MiniRange;
        float length = std::sqrt(mx * mx + my * my);
        if (length > 0.94f)
        {
            if (!clamp)
            {
                return;
            }

            mx *= 0.94f / length;
            my *= 0.94f / length;
        }

        float* dot = &map.dots[map.count * 4];
        dot[0] = mx;
        dot[1] = my;
        dot[2] = static_cast<float>(kind);
        dot[3] = 0.0f;
        map.count++;
    };

    // The other player first (it's drawn over the rest, at the rim when far), the ping, then the things
    if (other != nullptr && other->place != nullptr && other->chunk == centre->chunk)
    {
        other->place->SyncPosition();
        put(other->place->position.x, other->place->position.z, MiniOther, true);
    }

    float pingX = 0.0f;
    float pingZ = 0.0f;
    if (PingPlace(&pingX, &pingZ))
    {
        put(pingX, pingZ, MiniPing, true);
    }

    for (const MiniThing& thing : g_MiniThings)
    {
        put(thing.x, thing.z, thing.kind, false);
    }

    // Which way the centre's character faces on the circle (its forward axis over the camera's)
    RotateAndTranslate(centre->place);
    const Matrix4x4& m = centre->place->matrix;
    float faceX = m.m[2][0];
    float faceZ = m.m[2][2];
    map.facing = std::atan2(faceX * rightX + faceZ * rightZ, faceX * forwardX + faceZ * forwardZ);
    map.valid = true;
    // $TWIN_P2_LOG (tests): what's on the circle, every 2 seconds
    static const bool log = std::getenv("TWIN_P2_LOG") != nullptr;
    static u32 calls = 0;
    if (log && view == 0 && ++calls % 120 == 0)
    {
        int kinds[7] = {};
        for (int i = 0; i < map.count; i++)
        {
            kinds[static_cast<int>(map.dots[i * 4 + 2])]++;
        }

        Native::Log("second player: minimap view 0: %d dots (%d enemies, %d crates, %d wumpa, %d characters, other player %d), "
                    "facing %.2f; %zu things listed",
                    map.count, kinds[1], kinds[2], kinds[3], kinds[4], kinds[5], map.facing, g_MiniThings.size());
    }
}

void MinimapButtons()
{
    static bool was[2] = {false, false};
    bool down[2] = {false, false};
    const bool* keys = SDL_GetKeyboardState(nullptr);
    down[0] = keys != nullptr && keys[SDL_SCANCODE_M];
    auto* pads = static_cast<GamePadController*>(G_GamePadController);
    if (Active() && pads != nullptr && pads->pads[0] != nullptr && pads->pads[0]->info != nullptr)
    {
        down[0] = down[0] || (pads->pads[0]->now & (1u << PadUp)) != 0;
    }

    GamePad* second = pads != nullptr ? pads->pads[1] : nullptr;
    if (second != nullptr && second->info != nullptr)
    {
        down[1] = (second->now & (1u << PadUp)) != 0;
    }

    for (int player = 0; player < 2; player++)
    {
        if (down[player] && !was[player])
        {
            g_MiniWanted[player] = !g_MiniWanted[player];
        }

        was[player] = down[player];
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
    // The picture's height (-1 to 1: 2) a world unit takes at the ping's depth, and how flat a ring on the ground is seen (1 from
    // straight above, 0 from the side)
    float scale = 0.02f;
    float squash = 0.5f;
};
PingScreen g_PingScreen[2];
constexpr float PingSeconds = 7.0f;
constexpr float PingReach = 250.0f;

// A short two-note notification, made here (not one of the game's sounds): a soft "plim" played through its own audio stream
void PlayPingChime()
{
    static SDL_AudioStream* stream = nullptr;
    static bool tried = false;
    if (!tried)
    {
        tried = true;
        SDL_AudioSpec spec = {};
        spec.format = SDL_AUDIO_S16;
        spec.channels = 1;
        spec.freq = 44100;
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (stream != nullptr)
        {
            SDL_ResumeAudioStreamDevice(stream);
        }
    }

    if (stream == nullptr)
    {
        return;
    }

    constexpr int Rate = 44100;
    constexpr int Samples = Rate * 22 / 100;
    static std::vector<s16> tone;
    if (tone.empty())
    {
        tone.resize(Samples);
        for (int i = 0; i < Samples; i++)
        {
            float t = static_cast<float>(i) / Rate;
            // Two notes (an octave and a fifth up), the second from 70 ms on, each fading out fast
            float first = std::sin(2.0f * 3.14159265f * 880.0f * t) * std::exp(-t * 22.0f);
            float later = t >= 0.07f ? std::sin(2.0f * 3.14159265f * 1318.5f * (t - 0.07f)) * std::exp(-(t - 0.07f) * 20.0f) : 0.0f;
            float fadeIn = std::fmin(1.0f, t * 400.0f);
            tone[i] = static_cast<s16>(std::fmax(-1.0f, std::fmin(1.0f, (first * 0.6f + later * 0.5f) * fadeIn)) * 6500.0f);
        }
    }

    SDL_PutAudioStreamData(stream, tone.data(), static_cast<int>(tone.size() * sizeof(s16)));
}

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
    PlayPingChime();
    if (Active())
    {
        SendPingSpot(hit.x, hit.y, hit.z);
    }

    Native::Log("second player: player %d pinged (%.1f, %.1f, %.1f)", player + 1, hit.x, hit.y, hit.z);
}

// The small picture of the other player's view in a corner (a toggle: the first player's V key, the second's L3): who asked for it
bool g_PipWanted[2] = {false, false};

void PipButtons()
{
    MinimapButtons();
    static bool was[2] = {false, false};
    bool down[2] = {false, false};
    const bool* keys = SDL_GetKeyboardState(nullptr);
    down[0] = keys != nullptr && keys[SDL_SCANCODE_V];
    auto* pads = static_cast<GamePadController*>(G_GamePadController);
    GamePad* second = pads != nullptr ? pads->pads[1] : nullptr;
    if (second != nullptr && second->info != nullptr)
    {
        down[1] = (second->now & (1u << PadL3)) != 0;
    }

    // $TWIN_P2_TEST_PIP=seconds (tests): the first player's turned on then
    static const char* test = std::getenv("TWIN_P2_TEST_PIP");
    if (test != nullptr && static_cast<double>(SDL_GetTicks()) / 1000.0 >= std::atof(test))
    {
        g_PipWanted[0] = true;
    }

    for (int player = 0; player < 2; player++)
    {
        if (down[player] && !was[player])
        {
            g_PipWanted[player] = !g_PipWanted[player];
            Native::Log("second player: player %d's picture of the other %s", player + 1, g_PipWanted[player] ? "on" : "off");
        }

        was[player] = down[player];
    }
}

// Once a frame in play: the buttons that ping
void PingFrame()
{
    PipButtons();
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
    if (Active() && pads != nullptr && pads->pads[0] != nullptr && pads->pads[0]->info != nullptr)
    {
        down[0] = down[0] || (pads->pads[0]->now & (1u << PadR3)) != 0;
    }

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
    out.scale = projection.m[1][1] / cz;
    out.squash = std::fmax(0.18f, std::fmin(1.0f, std::fabs(toCamera.m[1][2])));
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

void ReceivePingSpot(float x, float y, float z)
{
    g_Ping.active = true;
    g_Ping.at = {x, y, z, 1.0f};
    g_Ping.since = SDL_GetTicksNS();
    PlayPingChime();
}

bool PingPlace(float* x, float* z)
{
    if (!g_Ping.active || static_cast<float>(SDL_GetTicksNS() - g_Ping.since) / 1e9f > PingSeconds)
    {
        return false;
    }

    *x = g_Ping.at.x;
    *z = g_Ping.at.z;
    return true;
}

bool MinimapDots(int view, const float** dots, int* count, float* facing)
{
    if (view < 0 || view > 1 || !g_Minimap[view].valid)
    {
        return false;
    }

    *dots = g_Minimap[view].dots;
    *count = g_Minimap[view].count;
    *facing = g_Minimap[view].facing;
    return true;
}

bool PipWanted(int viewer)
{
    return viewer >= 0 && viewer <= 1 && g_PipWanted[viewer];
}

bool PingMarker(int view, float* x, float* y, float* strength, bool* edge, float* scale, float* squash)
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
    *scale = g_PingScreen[view].scale;
    *squash = g_PingScreen[view].squash;
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
    if ((Wanted() || Active()) && OrbitCameraWanted())
    {
        OrbitCameras();
    }

    g_Drawings++;
    // The minimap's things, listed a few times a second
    if ((g_MiniWanted[0] || g_MiniWanted[1]) && (Wanted() || Active()) && g_Drawings % 4 == 1)
    {
        ListMiniThings();
    }

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
    // The ping's place in the picture just drawn (the first player's view 0, the second's 1), and the minimap's dots
    ProjectPing(g_Lent ? 1 : 0);
    ProjectMinimap(g_Lent ? 1 : 0);
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
    // Each window's own rate (2026-11-11, the players' ask: the first player's screen is 180 Hz, the second's picture goes through
    // Parsec at 60): the next frame is the view whose turn comes first, and it starts at that turn (TakeTwoViewsPace). Every other
    // frame, the first window had 60 like the second. The second view's turns a quarter of the first's period after them, so the
    // two never fall on the same moment
    if (Wanted())
    {
        u64 now = SDL_GetTicksNS();
        // The first window at the second's rate while a frame's work doesn't fit the two rates together (looking at the chickens by the
        // nitro at the start, a frame took 6.8 ms of the 6.7 that 90 + 60 leave), its own again once it fits with room for 3 s. The
        // frame's work: TakeTwoViewsPace's, from a frame's start to the next wait for the vertical blank, the render thread's wait in
        // it. (Measured from that wait to here, just after it, it was always about 0: the window never went down to 60, 110 frames a
        // second in the boss fight with 8.5 ms of work, 2026-11-11)
        {
            const double full = 1e9 / static_cast<double>(ViewRate(0) + ViewRate(1));
            if (g_FrameWork != 0 && g_FrameWork < 100000000ull)
            {
                g_FrameCost = g_FrameCost * 0.95 + static_cast<double>(g_FrameWork) * 0.05;
            }

            // (Not with the first window as fast as it goes, $TWIN_P1_FPS from 480: it never fits a budget it doesn't have, and it
            // would be at the second's 60 for good. The second window at Parsec's 60 and the first with the rest, 2026-11-12)
            const bool firstUnpaced = ViewRate(0) >= 480;
            if (firstUnpaced)
            {
                g_Reduced = false;
            }
            else if (!g_Reduced && g_FrameCost > full * 0.95 && ViewRate(0) > ViewRate(1))
            {
                g_Reduced = true;
                g_FitsSince = 0;
                Native::Log("two players: a frame's work %.1f ms, more than %d + %d leave: window 1 at %d for now", g_FrameCost / 1e6,
                            ViewRate(0), ViewRate(1), ViewRate(1));
            }
            else if (g_Reduced)
            {
                if (g_FrameCost < full * 0.75)
                {
                    g_FitsSince = g_FitsSince == 0 ? now : g_FitsSince;
                    if (now - g_FitsSince > 3000000000ull)
                    {
                        g_Reduced = false;
                        Native::Log("two players: a frame's work %.1f ms: window 1 at %d again", g_FrameCost / 1e6, ViewRate(0));
                    }
                }
                else
                {
                    g_FitsSince = 0;
                }
            }
        }

        const int firstRate = g_Reduced ? ViewRate(1) : ViewRate(0);
        const u64 periods[2] = {1000000000ull / static_cast<u64>(firstRate), 1000000000ull / static_cast<u64>(ViewRate(1))};
        static u64 turns[2] = {0, 0};
        if (!on)
        {
            // One view (cutscenes, menus, pauses): the first window's rate, both windows showing it
            turns[1] = 0;
            g_TurnCount = 0;
            turns[0] = turns[0] == 0 || turns[0] + periods[0] < now ? now : turns[0];
            g_PaceAt = turns[0];
            turns[0] += periods[0];
            next = 0;
        }
        else
        {
            if (turns[1] == 0)
            {
                turns[0] = now;
                turns[1] = now + periods[0] / 4;
            }

            for (int view = 0; view < 2; view++)
            {
                // Fallen behind (a slow frame, a load): from now on, not a rush of frames to catch up
                if (turns[view] + periods[view] < now)
                {
                    turns[view] = now;
                }
            }

            // (A tie, the other view than the last: both behind every frame, as fast as it goes, both turns were "now" and the first
            // always won, the second window black, 2026-11-11)
            static int last = 1;
            next = turns[0] < turns[1] ? 0 : turns[1] < turns[0] ? 1 : 1 - last;
            last = next;
            // (Its picture is shown two frames later, graphics/display.cpp's ShowFrame: the frame whose start is paced now shows the
            // picture chosen two choices ago, so that one's turn paces it)
            g_Turns[g_TurnCount % 3] = turns[next];
            g_TurnCount++;
            g_PaceAt = g_TurnCount >= 3 ? g_Turns[(g_TurnCount - 3) % 3] : turns[next];
            turns[next] += periods[next];
        }
    }

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

unsigned long long TakeTwoViewsPace()
{
    u64 at = g_PaceAt;
    g_PaceAt = 0;
    // (When the frame will start: at its turn, or now when that's past; its work measured from there, TwoViewsFrame)
    u64 now = SDL_GetTicksNS();
    // The frame just done's work: from its start to here (the game's wait for the vertical blank, before the picture's shown)
    g_FrameWork = g_FrameStart != 0 && now > g_FrameStart ? now - g_FrameStart : 0;
    g_FrameStart = at > now ? at : now;
    return at;
}

int TwoViewsFrameRate()
{
    return Wanted() ? ViewRate(0) + ViewRate(1) : 0;
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
    // Where the rider got in (its area and place): the first player is put back there when the carrier leaves its area
    ChunkData* fromChunk = nullptr;
    Vector4 from = {0.0f, 0.0f, 0.0f, 1.0f};
};
Ride g_Ride;
// The carrier's place a frame ago (a jump between frames is another area's numbers, a teleport), forgotten at each ride's start:
// kept from the last ride, a new one ended at once, four times in a row (2026-11-11)
Vector4 g_CarrierWas = {0.0f, 0.0f, 0.0f, 1.0f};
InstanceContext* g_CarrierWasOf = nullptr;
// When a ride last started or ended: a press within a moment of it is ignored (the log showed ten changes a second at a boss)
u64 g_RideChanged = 0;
constexpr u64 RideDebounce = 400000000ull;

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

// A player's own gamepad in the game (pad 0 the first's, the second port's the second's)
GamePad* OwnPad(int player)
{
    auto* pads = static_cast<GamePadController*>(G_GamePadController);
    return pads == nullptr ? nullptr : (player == 0 ? pads->pads[0] : pads->pads[1]);
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

    g_RideChanged = SDL_GetTicksNS();
    g_CarrierWasOf = nullptr;
    g_Ride.rider = rider;
    g_Ride.instance = instance;
    g_Ride.pad = controls->pad;
    g_Ride.collision = instance->flags.collisionActive;
    g_Ride.fromChunk = instance->chunk;
    if (instance->place != nullptr)
    {
        instance->place->SyncPosition();
        g_Ride.from = instance->place->position;
    }

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

    g_RideChanged = SDL_GetTicksNS();

    InstanceContext* instance = g_Ride.instance;
    if (instance != nullptr && InstanceLive(instance) && instance == PlayerOf(g_Ride.rider))
    {
        ControlsNode* controls = ControlsOf(instance);
        CharacterAgent* agent = AgentOf(instance);
        if (controls != nullptr && controls->pad == nullptr)
        {
            // (Its own pad when it had none as it got in: a cutscene's commands take a player's pad away, and giving that back left
            // the player unable to move once out of the backpack)
            controls->pad = g_Ride.pad != nullptr ? g_Ride.pad : OwnPad(g_Ride.rider);
        }

        if (agent != nullptr)
        {
            agent->eventFlags.eventsOff = 0;
            agent->buttons.locked.value = 0;
        }

        instance->flags.collisionActive = g_Ride.collision;
        // Out beside the carrier (a little above: never under the ground); not when it's in another area of the map (its numbers
        // are that area's: put there, it landed anywhere)
        InstanceContext* carrier = PlayerOf(1 - g_Ride.rider);
        if (carrier != nullptr && carrier->place != nullptr && carrier->chunk == instance->chunk)
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
    bool down = false;
    if (pad != nullptr && pad->info != nullptr)
    {
        // Select (View, Back), or both shoulders at once (L1 and R1: a pad whose Select didn't reach the game, through Parsec
        // for one: the second player never got into the first's backpack by its button)
        constexpr u32 Shoulders = (1u << PadL1) | (1u << PadR1);
        down = (pad->now & (1u << PadSelect)) != 0 || (pad->now & Shoulders) == Shoulders;
    }

    if (player == 0)
    {
        const bool* keys = SDL_GetKeyboardState(nullptr);
        down = down || (keys != nullptr && keys[SDL_SCANCODE_F5]);
    }

    bool pressed = down && !was[player];
    was[player] = down;
    // Not in the first moments of play: a button held through a cutscene (to skip it; the menus' and cutscenes' pad has both
    // players' buttons in it) would count as a press the moment play came back, and a player ended up in the other's backpack
    static u64 lastCall = 0;
    static int steady = 0;
    u64 now = SDL_GetTicksNS();
    if (player == 0)
    {
        steady = now - lastCall > 200000000ull ? 0 : steady + 1;
        lastCall = now;
    }

    if (steady < 30)
    {
        pressed = false;
    }

    if (pressed)
    {
        Native::Log("second player: the backpack button pressed by player %d", player + 1);
    }

    return pressed;
}

// The leash: the world is loaded around the first player only, so the two can't go far apart. Past WarnAt the windows' edges turn
// red (LeashWarning: stronger nearer the limit), at LimitAt the second player is taken into the first's backpack
// (20 and 30 were too short: the players were pulled together all the time)
// (Four times the 55 and 75 before, as the players asked)
constexpr float WarnAt = 220.0f;
constexpr float LimitAt = 300.0f;
float g_Leash = 0.0f;

// The second player let go and made again (in the first player's area and backpack): when it ended up in another area of the map
// than the first for good (the area's numbers aren't the first's: nothing between them says how far apart they are)
void RetireSecond(const char* why)
{
    Native::Log("second player: made again (%s)", why);
    EndRide();
    InstanceContext* old = g_Second.instance;
    if (old != nullptr && InstanceLive(old))
    {
        old->flags.visible = 0;
        old->flags.collisionActive = 0;
        if (ControlsNode* controls = ControlsOf(old))
        {
            controls->pad = nullptr;
        }

        // Taken out of its chunk altogether (its cells, its nodes, the chunk's sleeping list), not put to sleep there: a return to
        // a checkpoint takes every sleeping instance of the chunks back to the game's start (ResetChunkInstances), and the old
        // second player woke up there as a copy of the first player, where it was retired (2026-11-11: a cutscene player 2 set off
        // in another area, player 1 taken there, and at the next death a player 1 stood on that spot). In no chunk it's never
        // stepped, drawn or reset again. (Not released: its camera's rig and the game's handles still point at it)
        if (old->chunk != nullptr)
        {
            ChunkRemoveInstance(old->chunk, old);
        }
    }

    g_Second = Second();
}

// Since when the two players have been in different areas of the map (0: they're in the same one)
u64 g_ApartSince = 0;

void LeashFrame()
{
    g_Leash = 0.0f;
    InstanceContext* first = FirstPlayer();
    InstanceContext* second = SecondPlayer();
    if (first == nullptr || second == nullptr || first->place == nullptr || second->place == nullptr)
    {
        return;
    }

    // Another area of the map: no distance to measure (its numbers are that area's own). A few seconds of it and the second player
    // is made again beside the first; a moment of it is the walk across a door, which the game itself carries it through
    if (first->chunk != second->chunk)
    {
        // A neighbouring area the game steps in full (the areas next to the first player's: its loader steps every node kind
        // there) is play as usual: the second player walks on there, no leash (the areas' numbers can't be compared). Only an area
        // the game doesn't step in full (further away, or let go) makes it again beside the first, a moment later. (Made again
        // after 4 seconds in any other area, the second player was taken to the backpack walking across the hub's beach, hubc,
        // hubd and pier: 2026-11-11's log)
        bool stepped = second->chunk != nullptr && second->chunk->steppedKinds == ChunkData::EveryKindStepped;
        static bool loggedNeighbour = false;
        // Off the ground for 3 seconds in the next area: stuck there (in huba the second player hung in the air unable to move for a
        // minute, 2026-11-11): made again beside the first, in its backpack
        static u64 airSince = 0;
        CharacterAgent* secondAgent = AgentOf(second);
        bool onGround = secondAgent == nullptr || secondAgent->part == nullptr ||
                        static_cast<CharacterPart*>(secondAgent->part)->flags.onGround != 0;
        if (stepped && !onGround)
        {
            u64 now = SDL_GetTicksNS();
            airSince = airSince == 0 ? now : airSince;
            if (now - airSince > 3000000000ull)
            {
                airSince = 0;
                RetireSecond("stuck in the air in the next area");
                return;
            }
        }
        else
        {
            airSince = 0;
        }

        if (stepped)
        {
            g_ApartSince = 0;
            if (!loggedNeighbour)
            {
                loggedNeighbour = true;
                Native::Log("second player: in the next area (%s; the first in %s): plays on",
                            second->chunk->name.string != nullptr ? second->chunk->name.string : "?",
                            first->chunk != nullptr && first->chunk->name.string != nullptr ? first->chunk->name.string : "none");
            }

            return;
        }

        loggedNeighbour = false;
        u64 now = SDL_GetTicksNS();
        if (g_ApartSince == 0)
        {
            g_ApartSince = now;
            Native::Log("second player: in an area the game doesn't step (%s and %s)",
                        second->chunk != nullptr && second->chunk->name.string != nullptr ? second->chunk->name.string : "none",
                        first->chunk != nullptr && first->chunk->name.string != nullptr ? first->chunk->name.string : "none");
        }
        else if (now - g_ApartSince > 2000000000ull)
        {
            g_ApartSince = 0;
            RetireSecond("in an area the game doesn't step");
        }

        return;
    }

    g_ApartSince = 0;

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

bool PutFirstWhereSecond(InstanceContext* first, InstanceContext* second);

void BackpackFrame()
{
    g_Leash = 0.0f;
    bool pressed[2] = {BackpackPressed(0), BackpackPressed(1)};
    if (SDL_GetTicksNS() - g_RideChanged < RideDebounce)
    {
        pressed[0] = false;
        pressed[1] = false;
    }

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

    // $TWIN_P1_TEST_BACKPACK="in,out" (tests): the first player into the second's backpack that many seconds in, out at the second
    static const char* testFirst = std::getenv("TWIN_P1_TEST_BACKPACK");
    if (testFirst != nullptr)
    {
        static int stage = 0;
        double seconds = static_cast<double>(SDL_GetTicks()) / 1000.0;
        const char* comma = std::strchr(testFirst, ',');
        if ((stage == 0 && seconds >= std::atof(testFirst)) || (stage == 1 && comma != nullptr && seconds >= std::atof(comma + 1)))
        {
            stage++;
            pressed[0] = true;
        }
    }

    // On a vehicle with Cortex: the second player kept in the first's backpack (it walked about the ball's tunnels, made again at
    // every area, its camera all over: 2026-11-11)
    GameController* controller = G_GameController;
    static u32 pairingWas = 0;
    if (controller != nullptr && controller->progress.play.pairing != pairingWas)
    {
        pairingWas = controller->progress.play.pairing;
        Native::Log("second player: the pairing is %u%s", pairingWas, OnVehicle(controller) ? " (a vehicle: player 2 rides)" : "");
    }

    if (OnVehicle(controller) && SecondPlayer() != nullptr)
    {
        pressed[0] = false;
        pressed[1] = false;
        if (g_Ride.rider == 0)
        {
            EndRide();
        }

        if (g_Ride.rider < 0)
        {
            StartRide(1);
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
        // The carrier's place jumped (another area's numbers, a teleport) or it left the area: the first player isn't carried
        // there. The game loads the world around the first player only: carried with the second onto the boat to the Ice Lab, the
        // first was left over the water when the boat's area came, fell, and the game went back to the checkpoint before Tikimon
        Vector4& carrierWas = g_CarrierWas;
        InstanceContext*& carrierWasOf = g_CarrierWasOf;
        bool jumped = carrierWasOf == carrier && std::fabs(carrier->place->position.x - carrierWas.x) +
                                                         std::fabs(carrier->place->position.y - carrierWas.y) +
                                                         std::fabs(carrier->place->position.z - carrierWas.z) > 25.0f;
        carrierWas = carrier->place->position;
        carrierWasOf = carrier;
        if (g_Ride.rider == 0 && (jumped || carrier->chunk != g_Ride.instance->chunk))
        {
            // Out where it was last carried to, in its own area: the spot the carrier stood on just before it crossed (sent back to
            // where it got in, the first player took it for a return to the checkpoint without dying: 2026-11-11)
            EndRide();
            Native::Log("second player: player 2 left player 1's area (or jumped): player 1 out of the backpack where it was carried to");
            return;
        }

        if (carrier->chunk == g_Ride.instance->chunk && !jumped)
        {
            g_ApartSince = 0;
            PutAt(g_Ride.instance, carrier->place->position);
        }
        else
        {
            // The carrier crossed into another area: the rider's place can't be set from the carrier's numbers. Left a moment (the
            // game carries characters across a door), then, the second player made again in the carrier's area; the first player,
            // who can't be made again, let out of the backpack
            u64 now = SDL_GetTicksNS();
            if (g_ApartSince == 0)
            {
                g_ApartSince = now;
            }
            else if (now - g_ApartSince > 1000000000ull)
            {
                g_ApartSince = 0;
                if (g_Ride.rider == 1)
                {
                    RetireSecond("the carrier is in another area");
                }
                else
                {
                    EndRide();
                }

                return;
            }
        }

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
            // Player 1 into the backpack of a player 2 already in the next area: taken there first, as for a cutscene (the ride let
            // it out the same frame, the areas differing: by the caves' two natives, 2026-11-11)
            if (player == 0 && FirstPlayer() != nullptr && SecondPlayer() != nullptr &&
                FirstPlayer()->chunk != SecondPlayer()->chunk && !PutFirstWhereSecond(FirstPlayer(), SecondPlayer()))
            {
                Native::Log("second player: player 2's area can't be reached from player 1's: no ride");
                return;
            }

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

namespace
{
// Which player each thing follows (true: the second): kept, so a thing between the two (Aku Aku, an enemy, Cortex) doesn't flip
// from one to the other each step as it moves (Aku Aku jumped between them and showed on both)
std::unordered_map<const InstanceContext*, bool> g_Follows;

float DistanceBetween(ObjectPlace* a, ObjectPlace* b)
{
    a->SyncPosition();
    b->SyncPosition();
    float dx = a->position.x - b->position.x;
    float dy = a->position.y - b->position.y;
    float dz = a->position.z - b->position.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The player a thing follows: the one it follows now unless the other is well nearer (a quarter nearer and 3 units), the one
// whose backpack the other rides in alone while it's ridden (the rider isn't in the world)
bool FollowsSecond(const InstanceContext* thing, InstanceContext* first, InstanceContext* second)
{
    if (g_Ride.rider == 0 || (Active() && InPartnerBackpack()))
    {
        return true;
    }

    if (g_Ride.rider == 1 || (Active() && PartnerInMyBackpack()))
    {
        return false;
    }

    if (thing == second)
    {
        return true;
    }

    if (thing == first || thing->place == nullptr || first->place == nullptr || second->place == nullptr)
    {
        return false;
    }

    // The players in different areas of the map: a thing follows the one in its own area (the areas' numbers can't be compared)
    if (first->chunk != second->chunk)
    {
        return thing->chunk == second->chunk;
    }

    if (g_Follows.size() > 8192)
    {
        g_Follows.clear();
    }

    float toFirst = DistanceBetween(thing->place, first->place);
    float toSecond = DistanceBetween(thing->place, second->place);
    auto found = g_Follows.find(thing);
    if (found == g_Follows.end())
    {
        // Symmetric initial assignment across Host and Client: Player 1 (Host) initially owns equidistant followers
        found = g_Follows.emplace(thing, Active() && !IsHost()).first;
    }

    bool& follows = found->second;
    if (follows)
    {
        if (toFirst < toSecond - 3.0f && toFirst < toSecond * 0.75f)
        {
            follows = false;
        }
    }
    else if (toSecond < toFirst - 3.0f && toSecond < toFirst * 0.75f)
    {
        follows = true;
    }

    return follows;
}
}

InstanceContext* NearerPlayer(InstanceContext* from, InstanceContext* played)
{
    InstanceContext* first = LocalPlayer() != nullptr ? LocalPlayer() : played;
    // Online co-op: each PC runs its own player's world (the partner's game runs the partner's); the puppet is only drawn
    if (Active())
    {
        return first != nullptr ? first : played;
    }

    InstanceContext* second = g_SecondModel != nullptr ? SecondPlayer() : nullptr;
    if (from == nullptr || first == nullptr || second == nullptr || from->place == nullptr || first->place == nullptr ||
        second->place == nullptr || from->chunk == nullptr)
    {
        return first != nullptr ? first : played;
    }

    return FollowsSecond(from, first, second) ? second : first;
}

// "The player" for each thing the world steps (instances.cpp's ChunkInstances::Update, around each node's update): the game asks
// for the player everywhere (g_PlayerInstance, the progress's character played: a pickup flying to it, a worm launching it, an
// enemy hunting it, a trigger starting a cutscene, its own scripts), and only ever had one. While a node of an instance nearer the
// second player steps (or one of the second player's own), both point at the second player; given back after it. The first
// player's own nodes, and everything while the second rides in the backpack, see the first as always
namespace
{
Reference* g_SecondRef = nullptr;
InstanceContext* g_SecondRefFor = nullptr;
Reference* g_SavedPlayer = nullptr;
Reference* g_SavedCharacter = nullptr;
u32 g_SavedSlot = 0;
bool g_Swapped = false;
// The player's character and its part, kept twice by the game (player.h: the scripts' conditions, the node controllers' looks and
// animations, a worm's "the player stands on me"): the second player's for the same steps
PlayerCharacter* g_SavedCharacters[2] = {nullptr, nullptr};
CharacterPart* g_SavedParts[2] = {nullptr, nullptr};
PlayerCharacter* g_SecondCharacter = nullptr;
CharacterPart* g_SecondPart = nullptr;
// A cutscene set off by the second player is the second player's: the swap above kept through it (the second is "the player" for
// its script, its camera and the chunks' loading), given back to the first once play is back. The first player meanwhile rides in
// the second's backpack (the same area) or is benched where it stands (another area: its pad taken, its events off)
bool g_SecondLeads = false;
// Whether the game has been out of play since the lead started (the cutscene's state comes a frame after its script starts it), and
// when the lead started (still in play two seconds later: no cutscene came, the lead given back)
bool g_LeadOutOfPlay = false;
u64 g_LeadSince = 0;
struct Bench
{
    InstanceContext* instance = nullptr;
    GamePad* pad = nullptr;
};
Bench g_Bench;

// Once a frame: the reference to the second player kept (taken again when it's another, let go when it's gone)
void KeepSecondRef()
{
    InstanceContext* second = SecondPlayer();
    // (Online the partner is a puppet with no character of its own: it never borrows this player's, or whatever the world did to
    // "the player" near the puppet, a worm's launch, was done to this player far away)
    auto* node = second != nullptr ? static_cast<PlayerNode*>(GetGameNode(&second->nodes, NodeCharacter)) : nullptr;

    g_SecondCharacter = node != nullptr ? node->character : nullptr;
    g_SecondPart = g_SecondCharacter != nullptr ? static_cast<CharacterPart*>(g_SecondCharacter->part) : nullptr;
    if (second == g_SecondRefFor)
    {
        return;
    }

    if (g_SecondRef != nullptr)
    {
        RemoveReference(&g_SecondRef);
        g_SecondRef = nullptr;
    }

    g_SecondRefFor = second;
    g_SecondRef = second != nullptr ? AddReference(second) : nullptr;
}

float SquaredBetween(ObjectPlace* a, ObjectPlace* b)
{
    a->SyncPosition();
    b->SyncPosition();
    float dx = a->position.x - b->position.x;
    float dy = a->position.y - b->position.y;
    float dz = a->position.z - b->position.z;
    return dx * dx + dy * dy + dz * dz;
}
}

float SoundNearness(const Vector4* at, InstanceContext* listener)
{
    InstanceContext* second = g_SecondRefFor;
    if (at == nullptr || second == nullptr || listener == nullptr || listener->place == nullptr || g_Ride.rider == 1 ||
        (Active() && PartnerInMyBackpack()))
    {
        return 1.0f;
    }

    InstanceContext* ear = CameraOf(second);
    ear = ear != nullptr && ear->place != nullptr ? ear : second;
    if (ear->place == nullptr || ear->chunk != listener->chunk)
    {
        return 1.0f;
    }

    listener->place->SyncPosition();
    ear->place->SyncPosition();
    auto distance = [at](const Vector4& from)
    {
        float dx = at->x - from.x;
        float dy = at->y - from.y;
        float dz = at->z - from.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };

    float first = distance(listener->place->position);
    float near = distance(ear->place->position);
    return first > 0.01f && near < first ? near / first : 1.0f;
}

InstanceContext* SecondEarFor(ChunkData* chunk, InstanceContext* listener)
{
    if (!Wanted() || Active() || chunk == nullptr || !Valid() || g_Ride.rider == 1)
    {
        return nullptr;
    }

    InstanceContext* ear = CameraOf(g_Second.instance);
    if (ear == nullptr || ear->place == nullptr || ear->chunk != chunk || ear == listener ||
        (listener != nullptr && listener->chunk == chunk))
    {
        return nullptr;
    }

    return ear;
}

Reference* SecondPlayerReference()
{
    static const bool off = std::getenv("TWIN_ONE_FOCUS") != nullptr;
    if (off || Active())
    {
        return nullptr;
    }

    // A cutscene the second player leads: the game's focus is the second (the progress's character swapped), so the other focus is
    // the first player's, where it was benched (none while it rides in the second's backpack)
    if (g_SecondLeads)
    {
        InstanceContext* first = g_SavedPlayer != nullptr ? static_cast<InstanceContext*>(g_SavedPlayer->object) : nullptr;
        return first != nullptr && first->chunk != nullptr && g_Ride.rider != 0 ? g_SavedPlayer : nullptr;
    }

    InstanceContext* second = g_SecondRefFor;
    if (g_SecondRef == nullptr || second == nullptr || !Valid() || second != g_Second.instance || second->chunk == nullptr ||
        g_Ride.rider == 1)
    {
        return nullptr;
    }

    return g_SecondRef;
}

InstanceContext* LocalPlayer()
{
    Reference* ref = g_Swapped ? g_SavedPlayer : g_PlayerInstance;
    return ref != nullptr ? static_cast<InstanceContext*>(ref->object) : nullptr;
}

bool SwappedToSecond()
{
    return g_Swapped;
}

bool ShouldStepObject(const InstanceContext* instance)
{
    if (instance == nullptr)
    {
        return false;
    }

    if (Active() && IsReplayedObject(instance))
    {
        return true;
    }

    InstanceContext* second = g_SecondRefFor;
    if (second == nullptr || second->place == nullptr || instance->place == nullptr || instance->chunk != second->chunk)
    {
        return false;
    }

    if (g_Ride.rider == 1 || (Active() && PartnerInMyBackpack()))
    {
        return false;
    }

    return SquaredBetween(instance->place, second->place) <= 35.0f * 35.0f;
}

void BeginObjectUpdate(InstanceContext* owner)
{
    // $TWIN_P2_NO_SWAP (tests): the world sees the first player only, as before
    static const bool off = std::getenv("TWIN_P2_NO_SWAP") != nullptr;
    if (off || g_Swapped || g_SecondRef == nullptr || g_SecondRefFor == nullptr || owner == nullptr)
    {
        return;
    }

    // Online co-op: no swap to the partner's puppet. It has no character, no physics and no scripts, so whatever the world does to
    // "the player" while swapped (a worm's launch, a hit, the animation's controllers, the camera's rig) landed on this PC's own
    // player: the worm launched the player far away, the puppet copied this player's spin, the joining player's camera followed
    // the host. Each PC's world sees its own player; the partner's attacks, pickups, checkpoints and cutscenes come over the network
    if (Active())
    {
        return;
    }

    GameController* controller = G_GameController;
    InstanceContext* first = g_PlayerInstance != nullptr ? static_cast<InstanceContext*>(g_PlayerInstance->object) : nullptr;
    InstanceContext* second = g_SecondRefFor;
    if (controller == nullptr || first == nullptr || owner == first || second->place == nullptr || first->place == nullptr)
    {
        return;
    }

    // $TWIN_P2_SWAP_MODE (tests): 1 the second player's own nodes left out, 2 the cameras left out
    static const int mode = std::getenv("TWIN_P2_SWAP_MODE") != nullptr ? std::atoi(std::getenv("TWIN_P2_SWAP_MODE")) : 0;
    if (mode == 1 && owner == second)
    {
        return;
    }

    if (mode == 2 && GetGameNode(&owner->nodes, NodeCameraLens) != nullptr)
    {
        return;
    }

    // (Whose the thing is: the second player's, or the first's with the first the one who rides: the world then sees the carrier)
    if (!FollowsSecond(owner, first, second))
    {
        return;
    }

    GameProgress& progress = controller->progress;
    g_SavedPlayer = g_PlayerInstance;
    g_PlayerInstance = g_SecondRef;
    g_SavedSlot = progress.play.character;
    g_SavedCharacter = nullptr;
    Reference* played = g_SavedSlot < GameProgress::Characters ? progress.characters[g_SavedSlot] : nullptr;
    if (played != nullptr && played->object == first)
    {
        g_SavedCharacter = progress.characters[g_SavedSlot];
        progress.characters[g_SavedSlot] = g_SecondRef;
    }

    g_SavedCharacters[0] = g_PlayerCharacter;
    g_SavedCharacters[1] = g_PlayerCharacter2;
    g_SavedParts[0] = g_PlayerPart;
    g_SavedParts[1] = g_PlayerPart2;
    if (g_SecondCharacter != nullptr && g_SecondPart != nullptr)
    {
        g_PlayerCharacter = g_SecondCharacter;
        g_PlayerCharacter2 = g_SecondCharacter;
        g_PlayerPart = g_SecondPart;
        g_PlayerPart2 = g_SecondPart;
    }

    g_Swapped = true;
}

void EndObjectUpdate()
{
    // (Kept while the second player leads a cutscene: FinishLead gives it back)
    if (!g_Swapped || g_SecondLeads)
    {
        return;
    }

    g_Swapped = false;

    // (Given back unless the step itself changed them: then the game's own change stands)
    if (g_PlayerInstance == g_SecondRef)
    {
        g_PlayerInstance = g_SavedPlayer;
    }

    if (g_SecondCharacter != nullptr && g_PlayerCharacter == g_SecondCharacter)
    {
        g_PlayerCharacter = g_SavedCharacters[0];
        g_PlayerCharacter2 = g_SavedCharacters[1];
        g_PlayerPart = g_SavedParts[0];
        g_PlayerPart2 = g_SavedParts[1];
    }

    GameController* controller = G_GameController;
    if (controller != nullptr && g_SavedCharacter != nullptr && g_SavedSlot < GameProgress::Characters &&
        controller->progress.characters[g_SavedSlot] == g_SecondRef)
    {
        controller->progress.characters[g_SavedSlot] = g_SavedCharacter;
    }
}

InstanceContext* PlayerFor(InstanceContext* asking, InstanceContext* player)
{
    // (Anything asking, not only Cortex and Nina: a worm the second player jumped on launched the first, again and again)
    if (g_SecondModel == nullptr || asking == nullptr || player == nullptr)
    {
        return player;
    }

    // (A cutscene the second player leads: it's the player for everything in it, Cortex and Nina following the player included)
    if (g_SecondLeads && !Active() && Valid())
    {
        return g_Second.instance;
    }

    InstanceContext* first = LocalPlayer() != nullptr ? LocalPlayer() : player;
    if (asking == first || Active())
    {
        return first;
    }

    InstanceContext* second = SecondPlayer();
    return asking == second ? second : NearerPlayer(asking, first);
}

bool TintsGreen(const ModelNode* node)
{
    if (node == nullptr)
    {
        return false;
    }

    if (Active())
    {
        if (node == g_SecondModel)
        {
            return PartnerSkin() != 0;
        }

        InstanceContext* local = LocalPlayer();
        if (local != nullptr && node == ModelOf(local))
        {
            return LocalSkin() != 0;
        }

        return false;
    }

    return node == g_SecondModel;
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
    if (controller == nullptr || controller->State() != GameController::StatePlaying)
    {
        return 0.0f;
    }

    return Active() ? OnlineLeashWarning() : g_Leash;
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

int RiderPlayer()
{
    return g_Ride.rider;
}

// The checkpoint waits for both players: a script body that sets one (behaviours.cpp's ExecuteState asks HoldsBody first) is held
// while the other player isn't in its range; the player there is held still (their pad taken: the bottom line says so) until the
// other arrives or they cancel (X, or B on the pad: walking on, asked again when they come back into range)
namespace
{
struct CheckpointWait
{
    InstanceContext* checkpoint = nullptr;
    u64 asked = 0;
    int waiter = -1;
    bool cancelled[2] = {false, false};
    bool frozen[2] = {false, false};
    GamePad* pad[2] = {nullptr, nullptr};
    bool showing = false;
    // A cutscene's wait (the two players near each other) rather than a checkpoint's (both near it), and when it began
    bool cutscene = false;
    u64 since = 0;
};
CheckpointWait g_Wait;
// A cutscene let go whatever the other's place (when the other player can't be called into the backpack: another area)
bool g_CutsceneForce = false;
// How long a cutscene waits for the other player before they're called (put in the backpack of the one waiting)
constexpr u64 CutsceneWaitNs = 8000000000ull;
constexpr float CheckpointRange = 15.0f;
// A cutscene starts once the players are this near each other (or one rides in the other's backpack)
constexpr float CutsceneTogether = 30.0f;

void Unfreeze(int player)
{
    if (!g_Wait.frozen[player])
    {
        return;
    }

    g_Wait.frozen[player] = false;
    InstanceContext* instance = player == 0 ? FirstPlayer() : SecondPlayer();
    ControlsNode* controls = ControlsOf(instance);
    if (controls != nullptr && controls->pad == nullptr && g_Ride.rider != player)
    {
        controls->pad = g_Wait.pad[player];
    }
}

void EndWait()
{
    Unfreeze(0);
    Unfreeze(1);
    if (g_Wait.showing)
    {
        ShowBottomLine("");
    }

    g_Wait = CheckpointWait();
}

// The first player benched where it stands through a cutscene the second leads in another area: its pad taken and its events off
// (the world around it still steps: nothing hits it meanwhile), given back with FinishLead
void BenchFirst(InstanceContext* first)
{
    ControlsNode* controls = ControlsOf(first);
    CharacterAgent* agent = AgentOf(first);
    if (first == nullptr || controls == nullptr || agent == nullptr)
    {
        return;
    }

    g_Bench.instance = first;
    g_Bench.pad = controls->pad;
    controls->pad = nullptr;
    agent->moveInput = {0.0f, 0.0f, 0.0f, 1.0f};
    agent->eventFlags.eventsOff = 1;
}

void UnbenchFirst()
{
    InstanceContext* first = g_Bench.instance;
    if (first != nullptr && InstanceLive(first))
    {
        ControlsNode* controls = ControlsOf(first);
        if (controls != nullptr && controls->pad == nullptr)
        {
            controls->pad = g_Bench.pad != nullptr ? g_Bench.pad : OwnPad(0);
        }

        if (CharacterAgent* agent = AgentOf(first))
        {
            agent->eventFlags.eventsOff = 0;
            agent->buttons.locked.value = 0;
        }
    }

    g_Bench = Bench();
}

// The second player leads the cutscene it set off: the swap kept (it's "the player" for the script, the camera and the chunks'
// loading), the first player into its backpack in the same area (out beside it once play is back) or benched in another
void StartLead()
{
    InstanceContext* first = FirstPlayer();
    InstanceContext* second = SecondPlayer();
    g_SecondLeads = true;
    g_LeadOutOfPlay = false;
    g_LeadSince = SDL_GetTicksNS();
    if (g_Ride.rider == 1)
    {
        EndRide();
    }

    // (Benched where it stands, in any area: the players' ask, 2026-11-11. Taken in the second's backpack in the same area, the first
    // was put on the second's spot at the end. Already riding in the second's backpack, it rides on)
    if (g_Ride.rider < 0)
    {
        BenchFirst(first);
    }

    // The game's view the second player's follow camera from now (the progress's character is the second while it leads): the
    // cutscene's rig moves the camera shown, and the first player's stayed in the first player's area, showing what the first
    // player saw through the whole cutscene (2026-11-11)
    GameController* controller = G_GameController;
    if (controller != nullptr && second != nullptr && CameraOf(second) != nullptr &&
        controller->progress.Instance(controller->progress.play.character) == second)
    {
        ShowCamera(controller, GameController::CameraFollow, 1);
        g_Orbits[1].camera = nullptr;
    }

    Native::Log("second player: a cutscene set off by player 2 is player 2's (\"the player\" through it); player 1 %s",
                g_Ride.rider == 0 ? "rides in player 2's backpack" : "waits where it is");
}

// Play back after a cutscene the second player led (or the game left it another way: a level's load, a restart): "the player" the
// first again, the first out of the backpack beside the second or off the bench, and the first player's camera shown again
void FinishLead(const char* why)
{
    if (!g_SecondLeads)
    {
        return;
    }

    g_SecondLeads = false;
    EndObjectUpdate();
    UnbenchFirst();
    if (g_Ride.rider == 0)
    {
        EndRide();
    }

    // The first player's own camera shown again, of the kind the game shows now (its rig or the follow camera): the second's was
    // shown from the cutscene's start. (The follow camera forced whatever the game showed left window 1 stuck where the first player
    // had been, 2026-11-11)
    GameController* controller = G_GameController;
    InstanceContext* first = FirstPlayer();
    InstanceContext* firstCamera = CameraOf(first);
    if (controller != nullptr && firstCamera != nullptr && controller->State() == GameController::StatePlaying)
    {
        u32 kind = static_cast<u32>(controller->states.camera);
        AssignReference(&controller->view.cameraObject, firstCamera);
        ShowCamera(controller, kind == GameController::CameraGameRig ? GameController::CameraGameRig : GameController::CameraFollow,
                   1);
        Native::Log("camera: player 1's own camera shown again (%s)", kind == GameController::CameraGameRig ? "the game's rig"
                                                                                                         : "the follow camera");
    }

    g_Orbits[0].camera = nullptr;
    g_Orbits[1].camera = nullptr;
    Native::Log("second player: the cutscene player 2 led is over (%s): \"the player\" is player 1 again", why);
}
}

bool CheckpointWaitHolds(InstanceContext* checkpoint)
{
    GameController* controller = G_GameController;
    InstanceContext* first = FirstPlayer();
    InstanceContext* second = SecondPlayer();
    if (!Wanted() || controller == nullptr || controller->State() != GameController::StatePlaying || first == nullptr ||
        second == nullptr || checkpoint == nullptr || checkpoint->place == nullptr || first->place == nullptr ||
        second->place == nullptr)
    {
        return false;
    }

    bool near0 = DistanceBetween(checkpoint->place, first->place) < CheckpointRange;
    bool near1 = DistanceBetween(checkpoint->place, second->place) < CheckpointRange;
    if (near0 && near1)
    {
        // Both there: it goes through now
        EndWait();
        Native::Log("second player: checkpoint, both players there");
        return false;
    }

    if (g_Wait.checkpoint != checkpoint)
    {
        EndWait();
        g_Wait.checkpoint = checkpoint;
        Native::Log("second player: checkpoint waits for the other player");
    }

    g_Wait.asked = SDL_GetTicksNS();
    g_Wait.waiter = near0 ? 0 : (near1 ? 1 : -1);
    return true;
}

// A script body that starts a cutscene (coopgame.cpp's HoldsBody), the instance that runs it: true to hold it back until both
// players are near each other, the player nearer the instance (who set it off) held still meanwhile; false once they are, with the
// swap of "the player" undone so the cutscene's commands (locking the player's inputs, switching characters) act on the first
// player as in a game with one
bool CutsceneWaitHolds(InstanceContext* director)
{
    GameController* controller = G_GameController;
    InstanceContext* first = FirstPlayer();
    InstanceContext* second = SecondPlayer();
    if (!Wanted() || controller == nullptr || controller->State() != GameController::StatePlaying || first == nullptr ||
        second == nullptr || first->place == nullptr || second->place == nullptr)
    {
        return false;
    }

    // (2026-11-10, the players' ask: no more waiting. The wait held the player who set it off still, its pad taken, and a cutscene
    // set off by the second player, while the world took it for "the player", acted on the wrong one: both left where the first
    // stood, unable to move, the first player's pad on the second's character after Tikimon. Now a cutscene always starts at once,
    // as in a game with one player: the swap undone (it acts on the first player), the first player put where the second set it
    // off, and the second carried in the first's backpack through it; out beside the first a moment after play comes back)
    {
        bool bySecond = SwappedToSecond();
        EndWait();
        g_CutsceneForce = false;
        // (2026-11-11, the players' rule: a cutscene is whoever set it off's. Set off by the second player it's the second's, the
        // swap kept through it: the first player taken to the second, or the cutscene's camera at the first player's spot, left a
        // grey screen with no camera when the first couldn't be taken there from another area)
        static const bool oldWay = std::getenv("TWIN_CUTSCENE_FIRST") != nullptr;
        if (bySecond && !oldWay && !Active() && Valid())
        {
            StartLead();
            return false;
        }

        EndObjectUpdate();
        first = FirstPlayer();
        second = SecondPlayer();
        if (first == nullptr || second == nullptr || first->place == nullptr || second->place == nullptr)
        {
            return false;
        }

        if (g_Ride.rider == 0)
        {
            // The first rode in the second's backpack: out on the second's spot (where the cutscene was set off)
            EndRide();
        }
        else if (bySecond && g_Ride.rider < 0)
        {
            // Into the second's area first when it's another (the next one: through the link between them, as the game takes a
            // character through a door). Set off from huba with the first player on the beach, the cutscene showed huba and left
            // the first player on the beach, its camera away, unable to move (2026-11-11)
            if (!PutFirstWhereSecond(first, second))
            {
                Native::Log("second player: the cutscene's area can't be reached from the first player's: the first stays");
            }
        }

        if (g_Ride.rider < 0 && first->chunk == second->chunk)
        {
            StartRide(1);
        }

        Native::Log("second player: a cutscene starts%s; player 2 %s", bySecond ? " (set off by player 2: player 1 put there)" : "",
                    g_Ride.rider == 1 ? "in player 1's backpack" : "held (another area of the map)");
        return false;
    }

    bool together = g_Ride.rider >= 0 || g_CutsceneForce ||
                    (first->chunk == second->chunk && DistanceBetween(first->place, second->place) < CutsceneTogether);
    if (together)
    {
        g_CutsceneForce = false;
        if (g_Wait.cutscene)
        {
            Native::Log("second player: cutscene, both players there");
        }

        EndWait();
        EndObjectUpdate();
        return false;
    }

    if (!g_Wait.cutscene)
    {
        EndWait();
        g_Wait.cutscene = true;
        g_Wait.since = SDL_GetTicksNS();
        // (A marker that there's a wait: the cutscene's place isn't used)
        g_Wait.checkpoint = first;
        Native::Log("second player: a cutscene waits for the other player");
    }

    g_Wait.asked = SDL_GetTicksNS();
    int waiter = -1;
    if (director != nullptr && director->place != nullptr)
    {
        waiter = DistanceBetween(director->place, first->place) <= DistanceBetween(director->place, second->place) ? 0 : 1;
    }

    g_Wait.waiter = waiter;
    return true;
}

// Once a frame: the waiting player held still or let go, the line shown, the cancel buttons
void CheckpointWaitFrame()
{
    if (g_Wait.checkpoint == nullptr)
    {
        return;
    }

    // Not asked about for a moment (the body ran, or the script moved on): over
    if (SDL_GetTicksNS() - g_Wait.asked > 300000000ull)
    {
        EndWait();
        return;
    }

    static bool was[2] = {false, false};
    bool down[2] = {false, false};
    const bool* keys = SDL_GetKeyboardState(nullptr);
    down[0] = keys != nullptr && keys[SDL_SCANCODE_X];
    auto* pads = static_cast<GamePadController*>(G_GamePadController);
    GamePad* second = pads != nullptr ? pads->pads[1] : nullptr;
    if (second != nullptr && second->info != nullptr)
    {
        down[1] = (second->now & (1u << PadCircle)) != 0;
    }

    // A cutscene's wait ends one way: the other player called into the backpack of the one waiting (the waiter's X or B, or after
    // CutsceneWaitNs on its own). Walking off was a way out that left the cutscene's place behind and lost the cutscene
    if (g_Wait.cutscene)
    {
        static bool wasCall[2] = {false, false};
        int who = g_Wait.waiter >= 0 ? g_Wait.waiter : 0;
        bool call = (down[who] && !wasCall[who]) || SDL_GetTicksNS() - g_Wait.since > CutsceneWaitNs;
        wasCall[0] = down[0];
        wasCall[1] = down[1];
        if (call && g_Ride.rider < 0)
        {
            InstanceContext* waiting = who == 0 ? FirstPlayer() : SecondPlayer();
            InstanceContext* other = who == 0 ? SecondPlayer() : FirstPlayer();
            if (waiting != nullptr && other != nullptr && waiting->chunk == other->chunk)
            {
                Native::Log("second player: the cutscene calls player %d into player %d's backpack", 2 - who, who + 1);
                StartRide(1 - who);
            }
            else
            {
                // In another area: it can't be carried over; the cutscene goes on without waiting
                g_CutsceneForce = true;
            }
        }
    }

    for (int player = 0; player < 2; player++)
    {
        InstanceContext* instance = player == 0 ? FirstPlayer() : SecondPlayer();
        bool leaving = !g_Wait.cutscene && instance != nullptr && g_Wait.checkpoint->place != nullptr && instance->place != nullptr &&
                       DistanceBetween(g_Wait.checkpoint->place, instance->place) > CheckpointRange + 4.0f;
        if (leaving)
        {
            // Out of the range again: asks to wait the next time it's in
            g_Wait.cancelled[player] = false;
        }

        // (A checkpoint's wait can be cancelled to walk on; a cutscene's can't: X there calls the other, above)
        if (!g_Wait.cutscene && down[player] && !was[player] && g_Wait.waiter == player)
        {
            g_Wait.cancelled[player] = true;
        }

        was[player] = down[player];
        bool shouldFreeze = g_Wait.waiter == player && (g_Wait.cutscene || !g_Wait.cancelled[player]) && instance != nullptr &&
                            g_Ride.rider != player;
        if (shouldFreeze && !g_Wait.frozen[player])
        {
            ControlsNode* controls = ControlsOf(instance);
            if (controls != nullptr)
            {
                g_Wait.pad[player] = controls->pad;
                controls->pad = nullptr;
                g_Wait.frozen[player] = true;
            }
        }
        else if (!shouldFreeze)
        {
            Unfreeze(player);
        }

        if (g_Wait.frozen[player])
        {
            if (CharacterAgent* agent = AgentOf(instance))
            {
                agent->moveInput = {0.0f, 0.0f, 0.0f, 1.0f};
            }
        }
    }

    bool waiting = g_Wait.waiter >= 0 && !g_Wait.cancelled[g_Wait.waiter];
    if (g_Wait.cutscene)
    {
        // The cutscene's line shows for both (the other must come near or get into the backpack)
        ShowBottomLine(g_Wait.waiter == 1 ? "Aguardando o outro jogador para a cutscene (B chama ele)"
                                          : "Aguardando o outro jogador para a cutscene (X chama ele)");
        g_Wait.showing = true;
    }
    else if (waiting)
    {
        ShowBottomLine(g_Wait.waiter == 0 ? "Aguardando o outro jogador no checkpoint (X cancela)"
                                          : "Aguardando o outro jogador no checkpoint (B cancela)");
        g_Wait.showing = true;
    }
    else if (g_Wait.showing)
    {
        ShowBottomLine("");
        g_Wait.showing = false;
    }
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

    // (The one out of the cutscene: the second player, or the first in one the second leads. Hidden as the second in its own
    // cutscene, the second player showed only while the game was paused, 2026-11-11)
    InstanceContext* out = g_SecondLeads ? FirstPlayer() : SecondPlayer();
    return out != nullptr && node == ModelOf(out);
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

    return InCutscene() && instance == (g_SecondLeads ? FirstPlayer() : SecondPlayer());
}

namespace
{
// The first player put where the second is: across the link between their areas when they're different (the game's own way through
// a door: the place taken through the link's matrix, the instance into the linked area); false when there's no loaded link
bool PutFirstWhereSecond(InstanceContext* first, InstanceContext* second)
{
    if (first == nullptr || second == nullptr || first->place == nullptr || second->place == nullptr || first->chunk == nullptr ||
        second->chunk == nullptr)
    {
        return false;
    }

    if (first->chunk != second->chunk)
    {
        ChunkLinkData* link = FindLinkTo(first->chunk, second->chunk);
        if (link == nullptr || link->flags.linkedRm2Loaded == 0 || first->ChangeChunk(link) == nullptr ||
            first->chunk != second->chunk)
        {
            return false;
        }

        Native::Log("second player: the first player taken into the second's area (%s)",
                    second->chunk->name.string != nullptr ? second->chunk->name.string : "?");
    }

    second->place->SyncPosition();
    Vector4 to = second->place->position;
    to.y += 0.3f;
    PutAt(first, to);
    return true;
}

// When the second player comes out of the first's backpack after a cutscene (0: not waiting to)
u64 g_OutAfterCutscene = 0;
// The game went back to a checkpoint: the second player is made again from scratch once play is back
bool g_FreshAfterRestart = false;
// The second player made fresh after a cutscene: out of the backpack a second after it's made
bool g_OutWhenMade = false;

// Once a frame of play: a player's camera that stays far from its character (after Tikimon's fight the first player's view
// hung over the whole island, until a ride in the backpack set it again) is set again: the third person camera taken over
// afresh, and the game's follow camera shown again if that wasn't enough
void CameraWatchdog()
{
    GameController* controller = G_GameController;
    if (controller == nullptr || controller->State() != GameController::StatePlaying)
    {
        return;
    }

    // No view camera at all in play (a grey picture: after a cutscene set off in another area, 2026-11-11): the game's follow camera
    // shown again after a moment (with either camera, the game's or the third person one)
    static int none = 0;
    if (ViewCamera(controller) == nullptr && FirstPlayer() != nullptr)
    {
        if (++none == 60)
        {
            none = 0;
            ShowCamera(controller, GameController::CameraFollow, 1);
            g_Orbits[0].camera = nullptr;
            Native::Log("camera: no view camera in play: the game's follow camera shown again");
        }
    }
    else
    {
        none = 0;
    }

    if (!FreeCamera())
    {
        return;
    }

    static int far[2] = {0, 0};
    for (int player = 0; player < 2; player++)
    {
        InstanceContext* character = player == 0 ? FirstPlayer() : (Valid() ? g_Second.instance : nullptr);
        InstanceContext* camera = player == 0 ? ViewCamera(controller) : CameraOf(character);
        if (character == nullptr || camera == nullptr || character->place == nullptr || camera->place == nullptr ||
            RiderPlayer() == player)
        {
            far[player] = 0;
            continue;
        }

        // (In another area than its character counts as far: the camera left in huba by a cutscene, 2026-11-11)
        if (camera->chunk == character->chunk && DistanceBetween(character->place, camera->place) < 40.0f)
        {
            far[player] = 0;
            continue;
        }

        far[player]++;
        if (far[player] == 90)
        {
            g_Orbits[player].camera = nullptr;
            Native::Log("camera: player %d's camera far from its character for a moment: the third person camera set again",
                        player + 1);
        }
        else if (far[player] == 180)
        {
            far[player] = 0;
            if (player == 0)
            {
                ShowCamera(controller, GameController::CameraFollow, 1);
                g_Orbits[0].camera = nullptr;
                Native::Log("camera: player 1's camera still far: the game's follow camera shown again");
            }
        }
    }
}

// Once a frame of play: each player's pad on its own character, and "the player" the first's out of the world's steps. After
// Tikimon's fight the first player's pad was on the second's character and the first stood still: whatever moved them, it's put
// back here (and logged)
void KeepPlayersApart()
{
    InstanceContext* first = FirstPlayer();
    InstanceContext* second = Valid() ? g_Second.instance : nullptr;
    if (first == nullptr || second == nullptr)
    {
        return;
    }

    GameController* controller = G_GameController;
    if (!g_Swapped && controller != nullptr && first == second)
    {
        GameProgress& progress = controller->progress;
        u32 slot = progress.play.character;
        Reference* played = slot < GameProgress::Characters ? progress.characters[slot] : nullptr;
        if (played != nullptr && played->object != nullptr && played->object != second)
        {
            g_PlayerInstance = played;
            first = static_cast<InstanceContext*>(played->object);
            Native::Log("second player: \"the player\" was the second player out of the world's steps: given back to the first");
        }
    }

    auto* firstNode = static_cast<PlayerNode*>(GetGameNode(&first->nodes, NodeCharacter));
    auto* secondNode = static_cast<PlayerNode*>(GetGameNode(&second->nodes, NodeCharacter));
    if (!g_Swapped && firstNode != nullptr && secondNode != nullptr && secondNode->character != nullptr &&
        g_PlayerCharacter == secondNode->character && firstNode->character != nullptr)
    {
        g_PlayerCharacter = firstNode->character;
        g_PlayerPart = static_cast<CharacterPart*>(firstNode->character->part);
        Native::Log("second player: the game's player character was the second's out of the world's steps: given back to the first");
    }

    ControlsNode* firstControls = ControlsOf(first);
    ControlsNode* secondControls = ControlsOf(second);
    GamePad* firstPad = OwnPad(0);
    GamePad* secondPad = OwnPad(1);
    if (secondControls != nullptr && firstPad != nullptr && secondControls->pad == firstPad && firstPad != secondPad)
    {
        secondControls->pad = g_Ride.rider == 1 ? nullptr : secondPad;
        Native::Log("second player: the second's character had the first player's pad: given its own");
    }

    if (firstControls != nullptr && secondPad != nullptr && firstControls->pad == secondPad && firstPad != secondPad)
    {
        firstControls->pad = g_Ride.rider == 0 ? nullptr : firstPad;
        Native::Log("second player: the first's character had the second player's pad: given its own");
    }
}
}

void SecondPlayerFrame()
{
    if (!Wanted())
    {
        if (Active())
        {
            g_SecondModel = ModelOf(SecondPlayer());
            KeepSecondRef();
            PingFrame();
        }

        return;
    }

    g_SecondModel = ModelOf(SecondPlayer());
    KeepSecondRef();
    PingFrame();
    CheckpointWaitFrame();

    // $TWIN_P2_TEST_DIE=seconds (tests): at that time the lives are emptied and the game restarts from the checkpoint, as a death does
    static const char* testDie = std::getenv("TWIN_P2_TEST_DIE");
    static bool died = false;
    if (testDie != nullptr && !died && G_GameController != nullptr && G_GameController->State() == GameController::StatePlaying &&
        static_cast<double>(SDL_GetTicks()) / 1000.0 >= std::atof(testDie))
    {
        died = true;
        G_GameController->progress.counts.lives = 0;
        G_GameController->RestartFromCheckpoint();
    }

    GameController* controller = G_GameController;
    u32 state = controller != nullptr ? controller->State() : GameController::NoState;
    static int playingFrames = 0;
    // Out of play since it was there (a death's restart, a level's load): put beside the first player again once play is back, the
    // same character (made again only when it's gone: a second made each time stayed, a crowd of them after a few deaths)
    // Why the game fades out and restarts (a death of either player, or something else): logged where each player is when it does
    static u32 lastState = 99;
    if (state == GameController::StateFadingOut && lastState != GameController::StateFadingOut)
    {
        InstanceContext* first = FirstPlayer();
        InstanceContext* second = SecondPlayer();
        CharacterAgent* firstAgent = AgentOf(first);
        CharacterAgent* secondAgent = AgentOf(second);
        auto where = [](InstanceContext* instance, float* x, float* y, float* z) -> const char*
        {
            if (instance == nullptr || instance->place == nullptr)
            {
                return "none";
            }

            instance->place->SyncPosition();
            *x = instance->place->position.x;
            *y = instance->place->position.y;
            *z = instance->place->position.z;
            return instance->chunk != nullptr && instance->chunk->name.string != nullptr ? instance->chunk->name.string : "?";
        };
        float fx = 0, fy = 0, fz = 0, sx = 0, sy = 0, sz = 0;
        const char* firstChunk = where(first, &fx, &fy, &fz);
        const char* secondChunk = where(second, &sx, &sy, &sz);
        Native::Log("second player: the game fades out: first dead %d at (%.1f, %.1f, %.1f) in %s; second dead %d at (%.1f, %.1f, %.1f) in "
                    "%s; ride %d",
                    firstAgent != nullptr ? static_cast<int>(firstAgent->state.dead) : -1, fx, fy, fz, firstChunk,
                    secondAgent != nullptr ? static_cast<int>(secondAgent->state.dead) : -1, sx, sy, sz, secondChunk, g_Ride.rider);
    }

    lastState = state;
    static bool returning = false;
    // Whether a cutscene played since play was last there (the states between it and play, a load among them, don't take that
    // back): the second player comes back from it on foot, never into the backpack
    static bool fromCutscene = false;
    // The second player held still, hidden and out of the way (no collision) while a cutscene plays: its pad and collision kept
    static bool cutsceneHeld = false;
    static GamePad* heldPad = nullptr;
    static u32 heldCollision = 0;
    if (state != GameController::StatePlaying)
    {
        if (g_SecondLeads)
        {
            g_LeadOutOfPlay = true;
            // A cutscene the second player leads: it's in it as "the player", nothing to hold. Out of the cutscene another way than
            // back to play (a level's load, a restart): "the player" given back at once (the instances may go with the level)
            if (state != GameController::StateWatching && state != GameController::StatePaused &&
                state != GameController::StateWaiting && state != GameController::StateStartingPlay)
            {
                FinishLead("the game left the cutscene");
            }
            else
            {
                return;
            }
        }

        if (state == GameController::StateWatching && Valid())
        {
            // In the first player's backpack through the cutscene (the first riding in the second's: out first); held still and
            // out of the way below when it can't ride (another area of the map)
            if (g_Ride.rider == 0)
            {
                EndRide();
            }

            if (g_Ride.rider < 0 && FirstPlayer() != nullptr && FirstPlayer()->chunk == g_Second.instance->chunk)
            {
                StartRide(1);
            }

            returning = true;
            fromCutscene = true;
            playingFrames = 0;
            ControlsNode* controls = ControlsOf(g_Second.instance);
            if (g_Ride.rider != 1 && !cutsceneHeld && controls != nullptr)
            {
                cutsceneHeld = true;
                heldPad = controls->pad;
                heldCollision = g_Second.instance->flags.collisionActive;
                controls->pad = nullptr;
                g_Second.instance->flags.collisionActive = 0;
            }

            if (CharacterAgent* agent = AgentOf(g_Second.instance))
            {
                agent->moveInput = {0.0f, 0.0f, 0.0f, 1.0f};
            }
        }
        else if (state != GameController::StateWatching && state != GameController::StatePaused &&
                 state != GameController::StateWaiting)
        {
            // Back to a checkpoint (a death of either): the second player made again from scratch once play is back. Kept, its own
            // scripts went on with its death after the restart and asked for the checkpoint again every 4.4 s, forever (killed by a
            // nitro, 2026-11-11's log)
            if (state == GameController::StateFadingOut || state == GameController::StateRestarting)
            {
                g_FreshAfterRestart = true;
            }

            returning = true;
            if (!fromCutscene)
            {
                playingFrames = 0;
            }

            // (A death's restart, a level's load: both come back on foot)
            EndRide();
        }

        return;
    }

    // Back in play after a cutscene the second player led: "the player" the first again, both where they are (the first beside the
    // second when it rode in its backpack); the second player kept as it is (it played the cutscene itself: nothing to make again)
    if (g_SecondLeads && (g_LeadOutOfPlay || SDL_GetTicksNS() - g_LeadSince > 2000000000ull))
    {
        FinishLead(g_LeadOutOfPlay ? "play is back" : "no cutscene came");
        returning = false;
        fromCutscene = false;
        playingFrames = 0;
    }
    else if (g_SecondLeads)
    {
        // (The cutscene's state not there yet: nothing of play's runs meanwhile, the second player stays "the player")
        return;
    }

    // A safety net: a player whose pad has been taken away for two seconds of ordinary play (not in a backpack, not waiting, not
    // held for a cutscene) gets it back, and it's logged. Something took it and never gave it back (frozen, unable to move)
    {
        static int without[2] = {0, 0};
        for (int player = 0; player < 2; player++)
        {
            InstanceContext* instance = player == 0 ? FirstPlayer() : SecondPlayer();
            ControlsNode* controls = ControlsOf(instance);
            bool held = g_Ride.rider == player || g_Wait.frozen[player] || (player == 1 && cutsceneHeld);
            if (controls == nullptr || controls->pad != nullptr || held || OwnPad(player) == nullptr ||
                (player == 1 && OwnPad(1)->info == nullptr))
            {
                without[player] = 0;
                continue;
            }

            if (++without[player] > 120)
            {
                without[player] = 0;
                controls->pad = OwnPad(player);
                if (CharacterAgent* agent = AgentOf(instance))
                {
                    agent->buttons.locked.value = 0;
                }

                Native::Log("second player: player %d had no pad for two seconds of play: given back", player + 1);
            }
        }
    }

    // Play again after a cutscene: its pad and collision given back, its inputs unlocked (the cutscene's commands locked them while
    // "the player" was whichever the world saw), and where it stood kept unless the first player was taken far (then beside it)
    if (cutsceneHeld)
    {
        cutsceneHeld = false;
        if (Valid())
        {
            ControlsNode* controls = ControlsOf(g_Second.instance);
            if (controls != nullptr && controls->pad == nullptr)
            {
                controls->pad = heldPad;
            }

            g_Second.instance->flags.collisionActive = heldCollision;
            if (CharacterAgent* agent = AgentOf(g_Second.instance))
            {
                agent->buttons.locked.value = 0;
            }

            InstanceContext* first = FirstPlayer();
            if (first != nullptr && first->place != nullptr && g_Second.instance->place != nullptr &&
                DistanceBetween(first->place, g_Second.instance->place) > 12.0f)
            {
                first->place->SyncPosition();
                Vector4 to = first->place->position;
                to.y += 0.3f;
                PutAt(g_Second.instance, to);
                Native::Log("second player: back from the cutscene, put on the first player's spot");
            }
        }

        heldPad = nullptr;
    }

    // Back from a cutscene: the second player made again from scratch too, in the first's backpack and out beside it a second later.
    // A cutscene the second player set off ran the script's first commands while the world took it for "the player" (held in a pose,
    // its gravity off) and the game let only the first go after it: the second hung in the air, unable to move, the backpack
    // didn't free it (2026-11-11, the second cutscene by the beach)
    if (returning && Valid() && fromCutscene && !g_FreshAfterRestart)
    {
        fromCutscene = false;
        returning = false;
        playingFrames = 0;
        g_OutWhenMade = true;
        RetireSecond("back from a cutscene: made fresh");
        return;
    }

    if (returning && Valid() && g_FreshAfterRestart)
    {
        g_FreshAfterRestart = false;
        returning = false;
        playingFrames = 0;
        RetireSecond("back from a checkpoint: made fresh");
        return;
    }

    if (returning && Valid())
    {
        // Back in play: after a death or a level's load into the first player's backpack, carried from the first frame (put beside
        // the first it sometimes landed in a wall or under the ground, or stayed where it died, dying again); Select gets out. After a
        // cutscene it stays where it was when that's near the first player (the players need not ride at every cutscene), else the
        // backpack (the cutscene took the first player far)
        returning = false;
        playingFrames = 0;
        // (After a cutscene: on foot, handled above)
        bool after = fromCutscene;
        fromCutscene = false;
        if (!after && g_Ride.rider < 0)
        {
            StartRide(1);
            Native::Log("second player: back in play, in the first player's backpack");
        }

        if (after && g_Ride.rider == 1)
        {
            // Out beside the first player a second into play (the cutscene's own moves of the first done by then)
            g_OutAfterCutscene = SDL_GetTicksNS() + 1000000000ull;
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

        if (g_OutAfterCutscene != 0 && SDL_GetTicksNS() >= g_OutAfterCutscene)
        {
            g_OutAfterCutscene = 0;
            if (g_Ride.rider == 1)
            {
                EndRide();
                Native::Log("second player: out of the backpack beside player 1 after the cutscene");
            }
        }

        KeepPlayersApart();
        CameraWatchdog();
        BackpackFrame();
        return;
    }

    g_Second = Second();
    if (++playingFrames >= SettleFrames)
    {
        playingFrames = 0;
        Make();
        if (Valid())
        {
            if (fromCutscene)
            {
                // Made again after a cutscene (a level loaded): on the first player's spot, not in the backpack
                fromCutscene = false;
                InstanceContext* first = FirstPlayer();
                if (first != nullptr && first->place != nullptr && g_Second.instance->place != nullptr)
                {
                    first->place->SyncPosition();
                    Vector4 to = first->place->position;
                    to.y += 0.3f;
                    PutAt(g_Second.instance, to);
                }
            }
            else if (g_Ride.rider < 0)
            {
                // Made: in the first player's backpack to start with (never in a wall); Select gets out
                StartRide(1);
            }

            if (g_OutWhenMade && g_Ride.rider == 1)
            {
                // (Made fresh after a cutscene: out beside the first a second into play, as before)
                g_OutAfterCutscene = SDL_GetTicksNS() + 1000000000ull;
            }

            g_OutWhenMade = false;
        }
    }
}
}
