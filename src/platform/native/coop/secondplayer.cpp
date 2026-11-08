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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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

    ChunkEntry* chunk = ChunkOfIndex(g_ChunkManager, agent->chunkIndex);
    if (chunk == nullptr)
    {
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
int g_Drawing = 0;
}

int TwoViewsFrame()
{
    GameController* controller = G_GameController;
    InstanceContext* second = SecondPlayer();
    InstanceContext* firstCamera = CameraOf(FirstPlayer());
    InstanceContext* secondCamera = CameraOf(second);
    // (The game shows its own rig most of the time in play, or the played character's follow camera: either is the first view)
    bool on = controller != nullptr && second != nullptr && firstCamera != nullptr && secondCamera != nullptr &&
              controller->State() == GameController::StatePlaying &&
              (controller->states.camera == GameController::CameraFollow ||
               controller->states.camera == GameController::CameraGameRig);
    // The camera the first player sees, taken on the first player's frames (given back after each of the second's)
    static InstanceContext* firstView = nullptr;
    if (controller != nullptr && g_Drawing == 0)
    {
        firstView = controller->view.cameraObject != nullptr ? static_cast<InstanceContext*>(controller->view.cameraObject->object)
                                                             : nullptr;
    }

    if (firstView != nullptr && !InstanceLive(firstView))
    {
        firstView = nullptr;
    }

    firstCamera = firstView != nullptr ? firstView : firstCamera;
    // Why there's one view while there's a second player (once every 3 s)
    static u32 calls = 0;
    calls++;
    if (!on && second != nullptr && calls % 360 == 1)
    {
        Native::Log("second player: one view (first camera %s, second camera %s, state %u, camera kind %u)",
                    firstCamera != nullptr ? "there" : "none", secondCamera != nullptr ? "there" : "none",
                    controller != nullptr ? controller->State() : 99u,
                    controller != nullptr ? static_cast<unsigned>(controller->states.camera) : 99u);
    }

    int drawn = g_Drawing;
    int next = on ? 1 - g_Drawing : 0;
    if (next != g_Drawing && controller != nullptr && (next == 1 ? secondCamera : firstCamera) != nullptr)
    {
        AssignReference(&controller->view.cameraObject, next == 1 ? secondCamera : firstCamera);
    }

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
    return drawn;
}

void SecondPlayerFrame()
{
    if (!Wanted())
    {
        return;
    }

    GameController* controller = G_GameController;
    u32 state = controller != nullptr ? controller->State() : GameController::NoState;
    static int playingFrames = 0;
    if (state != GameController::StatePlaying)
    {
        // Out of play (loading, restarting, a menu): forgotten when it's gone; a cutscene or a pause keeps it
        if (state != GameController::StateWatching && state != GameController::StatePaused &&
            state != GameController::StateWaiting)
        {
            g_Second = Second();
            playingFrames = 0;
        }

        return;
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
        if (controls != nullptr && controls->pad == nullptr && pad != nullptr && pad->info != nullptr)
        {
            controls->pad = pad;
        }

        return;
    }

    g_Second = Second();
    if (++playingFrames >= SettleFrames)
    {
        playingFrames = 0;
        Make();
    }
}
}
