// The second player on this PC (coop.h): one world, two characters. The host's game makes a second character of the first player's
// object beside the first player, a character of its own (instancefactory.cpp's GameFactoryStandIn, while g_SpawningSecondPlayer:
// not the progress's, which stays the first player's), and gives it the second gamepad (the pads' port 1: platform/native/pads.cpp).
// A friend plays it from another PC through Parsec (their controller shows up here as a second gamepad).
//
// Asked for by coop.ini's players=2 or $TWIN_PLAYERS=2. Made again whenever play comes back (a level load, a checkpoint's restart).
#include "coop.h"

#include "native.h"
#include "ui/nativeui.h"

#include "game/agents.h"
#include "game/chunkdata.h"
#include "game/chunkloading.h"
#include "game/controllers.h"
#include "game/followcamera.h"
#include "game/gamecontroller.h"
#include "game/instancefactory.h"
#include "game/instances.h"
#include "game/pads.h"
#include "game/place.h"
#include "game/player.h"
#include "game/reference.h"

#include <SDL3/SDL.h>

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

void BeforeDraw()
{
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

    // 120 frames a second while there are two views (each window then 60)
    static bool rateSet = false;
    if (on && !rateSet)
    {
        rateSet = true;
        constexpr s32 Rate120 = 2;
        if (NativeUi::GetOption(NativeUi::OptionRefresh60) != Rate120)
        {
            NativeUi::SetOption(NativeUi::OptionRefresh60, Rate120);
            Native::Log("second player: two views, the frame rate set to 120 (each window 60)");
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
constexpr float WarnAt = 20.0f;
constexpr float LimitAt = 30.0f;
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
