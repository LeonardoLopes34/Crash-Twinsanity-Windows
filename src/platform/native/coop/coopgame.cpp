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
#include "game/animation.h"
#include "game/behaviours.h"
#include "game/memory.h"
#include "game/resources.h"
#include "game/chunkdata.h"
#include "game/commands.h"
#include "game/controllers.h"
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

ModelNode* ModelOf(InstanceContext* instance)
{
    return instance != nullptr ? static_cast<ModelNode*>(GetGameNode(&instance->nodes, NodeModel)) : nullptr;
}

// The game's table of models (OGIs) by their resource IDs: an ID's OGI, none when out of the table
GameOGI* OgiById(u16 id)
{
    ResourceTable* models = G_GameResourcesObjectPointer != nullptr ? G_GameResourcesObjectPointer->models : nullptr;
    if (models == nullptr || id == NoModelId || (id & ResourceIndexMask) >= models->bits.capacity)
    {
        return nullptr;
    }

    return static_cast<GameOGI*>(models->items[id & ResourceIndexMask]);
}

// An OGI's resource ID, when the table has it at that ID (else NoModelId)
u16 IdOfOgi(GameOGI* ogi)
{
    if (ogi == nullptr)
    {
        return NoModelId;
    }

    u16 id = static_cast<u16>(ogi->id);
    return OgiById(id) == ogi ? id : static_cast<u16>(NoModelId);
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

    // The position and turn brought up to date with the matrix first, as the game does before it reads them
    ObjectPlace* place = player->place;
    place->SyncPosition();
    place->SyncRotation();
    state.position[0] = place->position.x;
    state.position[1] = place->position.y;
    state.position[2] = place->position.z;
    state.yaw = YawOf(place->rotation);
    state.rotation[0] = place->rotation.x;
    state.rotation[1] = place->rotation.y;
    state.rotation[2] = place->rotation.z;
    state.rotation[3] = place->rotation.w;
    ModelNode* model = ModelOf(player);
    state.model = model != nullptr ? IdOfOgi(model->ogi) : static_cast<u16>(NoModelId);
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

    player->place->SyncPosition();
    Vector4 to = {x, y, z, 1.0f};
    if (player->place->MoveTo(&to))
    {
        QueueObject(player);
    }
}

// The partner drawn in this game: an instance of the partner's model alone (no object node, no physics, no scripts), as the
// cutscenes make theirs, placed where the partner is and posed as the partner's character is (ModelDrawn)
struct Puppet
{
    InstanceContext* instance = nullptr;
    ModelNode* node = nullptr;
    ChunkData* chunk = nullptr;
    u16 model = NoModelId;
};
Puppet g_Puppet;

void HidePuppet()
{
    if (g_Puppet.instance != nullptr && InstanceLive(g_Puppet.instance))
    {
        g_Puppet.instance->flags.visible = 0;
    }
}

InstanceContext* MakePuppet(ChunkData* chunk, GameOGI* ogi)
{
    auto* instance = InstanceContext::Construct(static_cast<InstanceContext*>(MemoryAllocate(sizeof(InstanceContext))));
    auto* node = ModelNode::Construct(static_cast<ModelNode*>(MemoryAllocate(sizeof(ModelNode))));
    node->SetOgi(ogi, 0, 0);
    instance->flags.visible = 1;
    RegisterNode(instance, AttachNode, node);
    node->AttachCollision();
    // Its joints made every frame (it's posed over them as it's drawn)
    node->bits.alwaysAnimated = 1;
    ChunkData::AddInstance(chunk, instance);
    return instance;
}

// The puppet where the partner is, when both are in the same area and the partner plays (not in the backpack, a cutscene or a
// load); hidden otherwise
void UpdatePuppet(double now, const PlayerState& local)
{
    GameController* controller = G_GameController;
    InstanceContext* player = PlayerInstance();
    PlayerState partner;
    bool show = g_Session.Connected() && g_Session.Partner(now, &partner) && player != nullptr && player->chunk != nullptr &&
                controller != nullptr && controller->State() == GameController::StatePlaying && partner.area == local.area &&
                partner.area != 0 && !partner.Has(PlayerState::InBackpack) && !partner.Has(PlayerState::Watching) &&
                !partner.Has(PlayerState::Loading);
    if (!show)
    {
        HidePuppet();
        return;
    }

    // The partner's model, or this player's when it isn't known here
    ModelNode* own = ModelOf(player);
    GameOGI* ogi = OgiById(partner.model);
    if (ogi == nullptr)
    {
        ogi = own != nullptr ? own->ogi : nullptr;
    }

    if (ogi == nullptr)
    {
        HidePuppet();
        return;
    }

    // Made again when it's gone (its chunk let go of it) or this player is in another chunk (the old one left hidden: its chunk
    // frees it)
    bool live = g_Puppet.instance != nullptr && InstanceLive(g_Puppet.instance);
    if (!live || g_Puppet.chunk != player->chunk)
    {
        if (live)
        {
            g_Puppet.instance->flags.visible = 0;
        }

        g_Puppet.instance = MakePuppet(player->chunk, ogi);
        g_Puppet.node = ModelOf(g_Puppet.instance);
        g_Puppet.chunk = player->chunk;
        g_Puppet.model = partner.model;
        Native::Log("coop: the partner's puppet made in %s (model %04X, %u joints)",
                    player->chunk->name.string != nullptr ? player->chunk->name.string : "?", partner.model,
                    static_cast<unsigned>(ogi->jointCount));
    }
    else if (g_Puppet.node != nullptr && g_Puppet.node->ogi != ogi)
    {
        // The partner switched character (Crash, Cortex, Nina)
        g_Puppet.node->SetOgi(ogi, 0, 0);
        g_Puppet.model = partner.model;
    }

    InstanceContext* instance = g_Puppet.instance;
    instance->flags.visible = 1;
    ObjectPlace* place = instance->place;
    place->SyncPosition();
    Vector4 position = {partner.position[0], partner.position[1], partner.position[2], 1.0f};
    bool moved = place->MoveTo(&position);
    place->SyncRotation();
    Vector4 rotation = {partner.rotation[0], partner.rotation[1], partner.rotation[2], partner.rotation[3]};
    bool turned = place->TurnTo(&rotation);
    if (moved || turned)
    {
        QueueObject(instance);
    }
}

// A pose packed for the network: the joints' count, the translations' scale, then each joint's turn (a unit quaternion in
// 16-bit parts), translation (16-bit parts of the scale) and its rows' lengths (64ths)
constexpr size_t PoseJointBytes = 4 * 2 + 3 * 2 + 3;
constexpr u32 MostPoseJoints = (MostPacket - 32) / PoseJointBytes;

s16 Pack16(float value, float scale)
{
    float scaled = value / scale;
    scaled = scaled > 32767.0f ? 32767.0f : (scaled < -32767.0f ? -32767.0f : scaled);
    return static_cast<s16>(std::lround(scaled));
}

void PackPose(const Matrix4x4* joints, u32 count, std::vector<uint8_t>* out)
{
    count = count < MostPoseJoints ? count : MostPoseJoints;
    out->clear();
    Writer writer(out);
    writer.U8(static_cast<uint8_t>(count));
    float reach = 0;
    for (u32 joint = 0; joint < count; joint++)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            reach = std::fmax(reach, std::fabs(joints[joint].m[3][axis]));
        }
    }

    float translationScale = reach > 0 ? reach / 32767.0f : 1.0f;
    writer.F32(translationScale);
    for (u32 joint = 0; joint < count; joint++)
    {
        const Matrix4x4& matrix = joints[joint];
        float r[3][3];
        float lengths[3];
        for (int row = 0; row < 3; row++)
        {
            lengths[row] = std::sqrt(matrix.m[row][0] * matrix.m[row][0] + matrix.m[row][1] * matrix.m[row][1] +
                                     matrix.m[row][2] * matrix.m[row][2]);
            float inverse = lengths[row] > 1e-6f ? 1.0f / lengths[row] : 0.0f;
            for (int column = 0; column < 3; column++)
            {
                r[row][column] = matrix.m[row][column] * inverse;
            }
        }

        // The rotation's quaternion (the pair of UnpackPose's matrix)
        float x, y, z, w;
        float trace = r[0][0] + r[1][1] + r[2][2];
        if (trace > 0)
        {
            float s = std::sqrt(trace + 1.0f) * 2.0f;
            w = 0.25f * s, x = (r[2][1] - r[1][2]) / s, y = (r[0][2] - r[2][0]) / s, z = (r[1][0] - r[0][1]) / s;
        }
        else if (r[0][0] > r[1][1] && r[0][0] > r[2][2])
        {
            float s = std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;
            w = (r[2][1] - r[1][2]) / s, x = 0.25f * s, y = (r[0][1] + r[1][0]) / s, z = (r[0][2] + r[2][0]) / s;
        }
        else if (r[1][1] > r[2][2])
        {
            float s = std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;
            w = (r[0][2] - r[2][0]) / s, x = (r[0][1] + r[1][0]) / s, y = 0.25f * s, z = (r[1][2] + r[2][1]) / s;
        }
        else
        {
            float s = std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;
            w = (r[1][0] - r[0][1]) / s, x = (r[0][2] + r[2][0]) / s, y = (r[1][2] + r[2][1]) / s, z = 0.25f * s;
        }

        for (float part : {x, y, z, w})
        {
            writer.U16(static_cast<uint16_t>(Pack16(part, 1.0f / 32767.0f)));
        }

        for (int axis = 0; axis < 3; axis++)
        {
            writer.U16(static_cast<uint16_t>(Pack16(matrix.m[3][axis], translationScale)));
        }

        for (float length : lengths)
        {
            float scaled = std::round(length * 64.0f);
            writer.U8(static_cast<uint8_t>(scaled > 255.0f ? 255.0f : scaled));
        }
    }
}

// A packed pose written over joints' matrices: false (nothing written) when it isn't for that many joints
bool UnpackPose(const std::vector<uint8_t>& pose, Matrix4x4* joints, u32 count)
{
    Reader reader(pose.data(), pose.size());
    u32 packed = reader.U8();
    float translationScale = reader.F32();
    if (!reader.Ok() || packed != count || reader.Left() < packed * PoseJointBytes)
    {
        return false;
    }

    for (u32 joint = 0; joint < packed; joint++)
    {
        float q[4];
        for (float& part : q)
        {
            part = static_cast<float>(static_cast<s16>(reader.U16())) / 32767.0f;
        }

        float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (length > 0)
        {
            for (float& part : q)
            {
                part /= length;
            }
        }

        float x = q[0], y = q[1], z = q[2], w = q[3];
        float r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                         {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                         {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
        float translation[3];
        for (float& value : translation)
        {
            value = static_cast<float>(static_cast<s16>(reader.U16())) * translationScale;
        }

        Matrix4x4& matrix = joints[joint];
        for (int row = 0; row < 3; row++)
        {
            float rowLength = static_cast<float>(reader.U8()) / 64.0f;
            for (int column = 0; column < 3; column++)
            {
                matrix.m[row][column] = r[row][column] * rowLength;
            }

            matrix.m[row][3] = 0.0f;
            matrix.m[3][row] = translation[row];
        }

        matrix.m[3][3] = 1.0f;
    }

    return reader.Ok();
}

// How many joints' matrices an animator has written this frame (its OGI's joints, when the matrices reach that far)
u32 JointsWritten(const OgiAnimator* animator, const GameOGI* ogi)
{
    if (animator == nullptr || ogi == nullptr || animator->matrices.start == nullptr)
    {
        return 0;
    }

    u32 count = ogi->jointCount;
    if (animator->matrices.end != nullptr && animator->matrices.end - animator->matrices.start < static_cast<ptrdiff_t>(count))
    {
        return 0;
    }

    return count;
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
    // $TWIN_COOP_TEST_OFFSET (tests on one PC, both players starting at the same place): the position sent moved along x
    static const float testOffset = static_cast<float>(std::atof(std::getenv("TWIN_COOP_TEST_OFFSET") != nullptr
                                                                    ? std::getenv("TWIN_COOP_TEST_OFFSET")
                                                                    : "0"));
    PlayerState sent = local;
    sent.position[0] += testOffset;
    g_Session.Update(now, sent);

    // The session's notices in the log too (connected, left...), and the link's round trip every 10 s while connected
    static std::string lastLogged;
    if (!g_Session.Notices().empty() && g_Session.Notices().back().text != lastLogged)
    {
        lastLogged = g_Session.Notices().back().text;
        Native::Log("coop: %s", lastLogged.c_str());
    }

    static double lastRoundTripLog = 0;
    if (g_Session.Connected() && now - lastRoundTripLog >= 10)
    {
        lastRoundTripLog = now;
        PlayerState partner;
        bool have = g_Session.Partner(now, &partner);
        Native::Log("coop: connected to %s, round trip %.0f ms, partner %s at (%.1f, %.1f, %.1f)%s",
                    g_Session.PartnerName().c_str(), g_Session.RoundTrip() * 1000.0, have ? "seen" : "not seen yet",
                    partner.position[0], partner.position[1], partner.position[2],
                    partner.Has(PlayerState::Watching) ? " watching a cutscene" : "");
    }

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

    UpdatePuppet(now, local);

    // The HUD: the cutscene's wait first, else the session's latest notice (connected, left...)
    std::string line = GateLine(now);
    if (line.empty() && !g_Session.Notices().empty())
    {
        line = g_Session.Notices().back().text;
    }

    ShowLine(line);
}

bool Active()
{
    StartOnce();
    return g_Session.GetRole() != Role::Off;
}

void ModelDrawn(ModelNode* node)
{
    if (g_Session.GetRole() == Role::Off || node == nullptr)
    {
        return;
    }

    // This player's character: its pose taken for the next state packet
    InstanceContext* player = PlayerInstance();
    if (player != nullptr && node == ModelOf(player))
    {
        u32 count = JointsWritten(node->drawnAnimator, node->drawnOgi);
        if (count != 0)
        {
            std::vector<uint8_t> pose;
            PackPose(node->drawnAnimator->matrices.start, count, &pose);
            g_Session.SetLocalPose(pose);
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                const Matrix4x4& root = node->drawnAnimator->matrices.start[0];
                Native::Log("coop: the player's pose: %u joints, %zu bytes, root joint at (%.2f, %.2f, %.2f)", count, pose.size(),
                            root.m[3][0], root.m[3][1], root.m[3][2]);
            }
        }

        return;
    }

    // The partner's puppet: posed as the partner's character, when the pose is for its model's joints
    if (node == g_Puppet.node && g_Puppet.instance != nullptr && InstanceLive(g_Puppet.instance))
    {
        u32 count = JointsWritten(node->drawnAnimator, node->drawnOgi);
        if (count != 0 && !g_Session.PartnerPose().empty())
        {
            UnpackPose(g_Session.PartnerPose(), node->drawnAnimator->matrices.start, count);
        }
    }
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
