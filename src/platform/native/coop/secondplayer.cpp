// The second player on this PC (coop.h): one world, two characters. The host's game makes a second character of the first player's
// object beside the first player, a character of its own (instancefactory.cpp's GameFactoryStandIn, while g_SpawningSecondPlayer:
// not the progress's, which stays the first player's), and gives it the second gamepad (the pads' port 1: platform/native/pads.cpp).
// A friend plays it from another PC through Parsec (their controller shows up here as a second gamepad).
//
// Asked for by coop.ini's players=2 or $TWIN_PLAYERS=2. Made again whenever play comes back (a level load, a checkpoint's restart).
#include "coop.h"

#include "native.h"

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

    auto* follow = static_cast<FollowNode*>(GetGameNode(&made->nodes, NodeFollow));
    if (follow != nullptr)
    {
        UnregisterNode(follow->owner, 0, follow);
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
