// Online co-op in the game (coop.h): the session (coopsession.cpp) tied to the game's player, scripts and HUD.
//
// Settings: $TWIN_COOP ("host" or "join"), $TWIN_COOP_JOIN (the host's address), $TWIN_COOP_PORT, $TWIN_COOP_NAME,
// $TWIN_COOP_SKIN (0 Crash, 1 Carbon Crash), $TWIN_COOP_LEASH (units; 0 none). coop.ini in the game's folder gives the same as
// key=value lines (role, join, port, name, skin, leash), the variables winning.
// Keys: F5 the backpack (in and out), F6 pull the partner over (offered at a cutscene's wait).
//
// Not done yet (COOP.md, "Next"): the partner drawn as a puppet instance of Crash (the session already has its interpolated state:
// Session::Partner), the world's events (crates, pickups, checkpoints) and the Carbon Crash look
#include "coop.h"

#include "coopsession.h"

#include "native.h"

#include "game/agentlab.h"
#include "game/behaviours.h"
#include "game/chunkdata.h"
#include "game/commands.h"
#include "game/gamecontroller.h"
#include "game/instances.h"
#include "game/objectnode.h"
#include "game/objects.h"
#include "game/place.h"
#include "game/player.h"
#include "game/reference.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace Coop
{
namespace
{
Session g_Session;
bool g_Started = false;
bool g_ShowingText = false;
std::string g_ShownText;
// The backpack's ride: behind and above the partner
constexpr float BackpackBehind = 0.6f;
constexpr float BackpackUp = 0.9f;
// Coming out: beside the partner
constexpr float LeaveSide = 1.2f;

std::map<std::string, std::string> ReadIni()
{
    std::map<std::string, std::string> values;
    FILE* file = std::fopen("coop.ini", "r");
    if (file == nullptr)
    {
        return values;
    }

    char line[256];
    while (std::fgets(line, sizeof(line), file) != nullptr)
    {
        std::string text = line;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        {
            text.pop_back();
        }

        size_t equals = text.find('=');
        if (text.empty() || text[0] == '#' || text[0] == ';' || equals == std::string::npos)
        {
            continue;
        }

        values[text.substr(0, equals)] = text.substr(equals + 1);
    }

    std::fclose(file);
    return values;
}

std::string Setting(const std::map<std::string, std::string>& ini, const char* key, const char* variable,
                    const char* fallback = "")
{
    const char* value = std::getenv(variable);
    if (value != nullptr && *value != '\0')
    {
        return value;
    }

    auto found = ini.find(key);
    return found != ini.end() ? found->second : fallback;
}

void StartOnce()
{
    if (g_Started)
    {
        return;
    }

    g_Started = true;
    auto ini = ReadIni();
    std::string role = Setting(ini, "role", "TWIN_COOP");
    Settings settings;
    if (role == "host")
    {
        settings.role = Role::Host;
    }
    else if (role == "join" || role == "client")
    {
        settings.role = Role::Client;
    }
    else
    {
        return;
    }

    settings.join = Setting(ini, "join", "TWIN_COOP_JOIN");
    settings.port = static_cast<uint16_t>(std::atoi(Setting(ini, "port", "TWIN_COOP_PORT", "27420").c_str()));
    settings.name = Setting(ini, "name", "TWIN_COOP_NAME", settings.role == Role::Host ? "Jogador 1" : "Jogador 2");
    settings.skin = static_cast<uint8_t>(std::atoi(Setting(ini, "skin", "TWIN_COOP_SKIN", settings.role == Role::Host ? "0" : "1").c_str()));
    settings.leashDistance = static_cast<float>(std::atof(Setting(ini, "leash", "TWIN_COOP_LEASH", "0").c_str()));
    if (!g_Session.Start(settings))
    {
        Native::Log("coop: not started: %s", g_Session.Error().c_str());
        return;
    }

    Native::Log("coop: started as the %s", settings.role == Role::Host ? "host" : "client");
}

InstanceContext* PlayerInstance()
{
    return g_PlayerInstance != nullptr ? static_cast<InstanceContext*>(g_PlayerInstance->object) : nullptr;
}

// The yaw of a quaternion (x, y, z, w), about the game's up axis (y)
float YawOf(const Vector4& rotation)
{
    return std::atan2(2.0f * (rotation.w * rotation.y + rotation.x * rotation.z),
                      1.0f - 2.0f * (rotation.y * rotation.y + rotation.x * rotation.x));
}

PlayerState LocalState()
{
    PlayerState state;
    GameController* controller = G_GameController;
    u32 gameState = controller != nullptr ? controller->State() : GameController::NoState;
    InstanceContext* player = PlayerInstance();
    if (player == nullptr || player->place == nullptr)
    {
        state.flags = PlayerState::Loading;
        return state;
    }

    const ObjectPlace* place = player->place;
    state.position[0] = place->position.x;
    state.position[1] = place->position.y;
    state.position[2] = place->position.z;
    state.yaw = YawOf(place->rotation);
    state.area = player->chunk != nullptr && player->chunk->name.string != nullptr ? HashKey(player->chunk->name.string) : 0;
    state.flags = PlayerState::Visible;
    if (gameState == GameController::StateWatching)
    {
        state.flags |= PlayerState::Watching;
    }

    if (gameState == GameController::StateLoadingLevel)
    {
        state.flags |= PlayerState::Loading;
    }

    return state;
}

// The local player's character put at a point (its place moved; the physics follow from it on the next frame)
void PutPlayerAt(float x, float y, float z)
{
    InstanceContext* player = PlayerInstance();
    if (player == nullptr || player->place == nullptr)
    {
        return;
    }

    player->place->position.x = x;
    player->place->position.y = y;
    player->place->position.z = z;
    player->place->MarkMoved();
    QueueObject(player);
}

void RideInBackpack(double now)
{
    PlayerState partner;
    if (!g_Session.Partner(now, &partner))
    {
        return;
    }

    float behindX = -std::sin(partner.yaw) * BackpackBehind;
    float behindZ = -std::cos(partner.yaw) * BackpackBehind;
    PutPlayerAt(partner.position[0] + behindX, partner.position[1] + BackpackUp, partner.position[2] + behindZ);
}

void LeaveBesidePartner(double now)
{
    PlayerState partner;
    if (!g_Session.Partner(now, &partner))
    {
        return;
    }

    float sideX = std::cos(partner.yaw) * LeaveSide;
    float sideZ = -std::sin(partner.yaw) * LeaveSide;
    PutPlayerAt(partner.position[0] + sideX, partner.position[1], partner.position[2] + sideZ);
}

// A key's press (down this frame, up the last)
bool Pressed(SDL_Scancode key)
{
    static bool down[SDL_SCANCODE_COUNT];
    const bool* state = SDL_GetKeyboardState(nullptr);
    bool now = state != nullptr && state[key];
    bool was = down[key];
    down[key] = now;
    return now && !was;
}

// The line at the bottom of the screen (the game's own bottom text), shown while there is one, hidden after. The game's font has
// no accents: the texts are written without them
void ShowLine(const std::string& text)
{
    GameController* controller = G_GameController;
    if (controller == nullptr)
    {
        return;
    }

    constexpr s32 Fade = 10;
    if (text.empty())
    {
        if (g_ShowingText)
        {
            controller->HideBottomText(Fade);
            g_ShowingText = false;
            g_ShownText.clear();
        }

        return;
    }

    if (!g_ShowingText)
    {
        g_ShowingText = controller->ShowBottomText(Fade) != 0;
    }

    if (g_ShowingText && text != g_ShownText)
    {
        StringAssign(&controller->oleg.textLine.text, text.c_str());
        g_ShownText = text;
    }
}

std::string GateLine(double now)
{
    Session::GateStatus status = g_Session.CutsceneStatus(now);
    const std::string& partner = g_Session.PartnerName();
    if (status.holding)
    {
        if (status.canPull)
        {
            return "Aguardando " + partner + "...  F6: puxar " + partner + " ate aqui";
        }

        return "Aguardando " + partner + "...";
    }

    if (status.partnerWaitsForMe)
    {
        return partner + " esta esperando voce  -  F6: ir ate " + partner;
    }

    return "";
}
}

void GameFrame()
{
    StartOnce();
    if (g_Session.GetRole() == Role::Off)
    {
        return;
    }

    double now = Now();
    PlayerState local = LocalState();
    g_Session.Update(now, local);

    // F5: the backpack in and out
    if (Pressed(SDL_SCANCODE_F5))
    {
        if (g_Session.InBackpack())
        {
            g_Session.LeaveBackpack();
            LeaveBesidePartner(now);
        }
        else
        {
            g_Session.EnterBackpack();
        }
    }

    // F6: pull the partner (or, when the partner waits at a cutscene, go to them: into their backpack)
    if (Pressed(SDL_SCANCODE_F6) && g_Session.Connected())
    {
        Session::GateStatus status = g_Session.CutsceneStatus(now);
        if (status.partnerWaitsForMe && !status.holding)
        {
            g_Session.EnterBackpack();
        }
        else
        {
            g_Session.PullPartner();
        }
    }

    // Pulled over, or too far for too long (the client): into the partner's backpack
    if (g_Session.TakePulled() || g_Session.LeashWantsBackpack(now, local))
    {
        g_Session.EnterBackpack();
    }

    if (g_Session.InBackpack())
    {
        RideInBackpack(now);
    }

    // The HUD: the cutscene's wait first, else the session's latest notice (connected, left...)
    std::string line = GateLine(now);
    if (line.empty() && !g_Session.Notices().empty())
    {
        line = g_Session.Notices().back().text;
    }

    ShowLine(line);
}

bool HoldsBody(const StateBody* body, BehaviourRunner* runner, BehaviourLevel* level)
{
    if (g_Session.GetRole() == Role::Off || body == nullptr)
    {
        return false;
    }

    bool startsCutscene = false;
    for (const ScriptCommand* command = body->commands; command != nullptr && !startsCutscene; command = command->next)
    {
        startsCutscene = command->vtable == g_CutsceneStartCommandVTable;
    }

    if (!startsCutscene)
    {
        return false;
    }

    // The cutscene's key: the director's object and the graph running, the same on both PCs (the same disc's scripts)
    std::string name = "?";
    auto* director = runner != nullptr ? static_cast<ObjectNodeBase*>(runner->agentNode) : nullptr;
    if (director != nullptr && director->object != nullptr && director->object->name.string != nullptr)
    {
        name = director->object->name.string;
    }

    if (level != nullptr && level->graph != nullptr && level->graph->name.string != nullptr)
    {
        name += "/";
        name.append(level->graph->name.string, level->graph->name.length);
    }

    uint32_t key = HashKey(name.c_str());
    Session::Gate gate = g_Session.AskCutscene(key, Now());
    if (gate == Session::Gate::Go)
    {
        Native::Log("coop: cutscene %s (%08X) starts", name.c_str(), key);
        ShowLine("");
        return false;
    }

    return true;
}
}
