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
#include "game/agents.h"
#include "game/animation.h"
#include "game/characters.h"
#include "game/collision.h"
#include "game/oleg.h"
#include "game/scenery.h"
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
Settings g_Settings;
bool g_Started = false;
bool g_ShowingText = false;
std::string g_ShownText;
// Coming out of the backpack: beside the partner
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
    g_Settings = settings;
    if (!g_Session.Start(settings))
    {
        Native::Log("coop: not started: %s (tried again every few seconds)", g_Session.Error().c_str());
        return;
    }

    Native::Log("coop: started as the %s", settings.role == Role::Host ? "host" : "client");
}

// The session that couldn't start (the port taken: the connection test or another game still open), tried again every 3 s, and
// said on the screen meanwhile
std::string RetryStart(double now)
{
    if (g_Session.GetLink() != Session::Link::Failed)
    {
        return "";
    }

    static double lastTry = 0;
    if (now - lastTry >= 3.0)
    {
        lastTry = now;
        if (g_Session.Start(g_Settings))
        {
            Native::Log("coop: started as the %s (on a retry)", g_Settings.role == Role::Host ? "host" : "client");
            return "";
        }
    }

    if (g_Settings.role == Role::Host)
    {
        return "Co-op: porta " + std::to_string(g_Settings.port) +
               " ocupada - feche o TESTAR CONEXAO ou o outro jogo aberto (tentando de novo...)";
    }

    return "Co-op: " + g_Session.Error();
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

// A model by what it is, the same on both PCs (its index in the game's tables isn't: the tables fill in the order things load):
// its joints, rigid models and skin, and its bounds
u32 OgiSignature(const GameOGI* ogi)
{
    if (ogi == nullptr)
    {
        return 0;
    }

    u32 hash = 2166136261u;
    auto mix = [&hash](u32 value) { hash = (hash ^ value) * 16777619u; };
    mix(ogi->jointCount);
    mix(ogi->rigidModelCount);
    mix(ogi->hasSkin);
    const f32* bounds = &ogi->bounds.min.x;
    for (int index = 0; index < 8; index++)
    {
        u32 bits;
        std::memcpy(&bits, &bounds[index], sizeof(bits));
        mix(bits);
    }

    return hash != 0 ? hash : 1;
}

// The model here with a signature: among the playable characters' (Crash, Cortex, Nina...), else none
GameOGI* OgiBySignature(u32 signature)
{
    GameController* controller = G_GameController;
    if (signature == 0 || controller == nullptr)
    {
        return nullptr;
    }

    for (u32 index = 0; index < GameProgress::Characters; index++)
    {
        ModelNode* node = ModelOf(controller->progress.Instance(index));
        if (node != nullptr && node->ogi != nullptr && OgiSignature(node->ogi) == signature)
        {
            return node->ogi;
        }
    }

    return nullptr;
}

// Whether the game is in play (or a cutscene, or paused over either): not loading, restarting, dying, on a menu
bool InPlay(u32 state)
{
    return state == GameController::StatePlaying || state == GameController::StateWatching ||
           state == GameController::StatePaused || state == GameController::StateWaiting;
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
    state.modelSignature = model != nullptr ? OgiSignature(model->ogi) : 0;
    state.area = player->chunk != nullptr && player->chunk->name.string != nullptr ? HashKey(player->chunk->name.string) : 0;
    state.flags = PlayerState::Visible;
    if (gameState == GameController::StateWatching)
    {
        state.flags |= PlayerState::Watching;
    }

    // Loading, dying, going back to a checkpoint, on a menu: where the character is now means nothing to the partner
    if (!InPlay(gameState))
    {
        state.flags |= PlayerState::Loading;
    }

    return state;
}

u32 AreaOfPlayer(const InstanceContext* player)
{
    return player != nullptr && player->chunk != nullptr && player->chunk->name.string != nullptr
               ? HashKey(player->chunk->name.string)
               : 0;
}

// The pause shared: who paused (only they resume), and whether this game's pause was the partner's asking
enum class PausedBy
{
    Nobody,
    Me,
    Partner,
};
PausedBy g_PausedBy = PausedBy::Nobody;
bool g_PausingForPartner = false;

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
    u32 signature = 0;
};
Puppet g_Puppet;

// Whether the puppet is still the instance made for it (live, and its model node the one made: a chunk let go, its memory may
// hold another instance at the same address)
bool PuppetValid()
{
    return g_Puppet.instance != nullptr && InstanceLive(g_Puppet.instance) &&
           GetGameNode(&g_Puppet.instance->nodes, NodeModel) == g_Puppet.node;
}

void HidePuppet()
{
    if (PuppetValid())
    {
        g_Puppet.instance->flags.visible = 0;
    }
}

// The puppet left to its chunk (hidden; the chunk frees it) and made again when play comes back: a level load, a death, a
// checkpoint's restart can each free or move what it was made in
void ForgetPuppet()
{
    if (g_Puppet.instance != nullptr)
    {
        HidePuppet();
        g_Puppet = Puppet();
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
    u32 state = controller != nullptr ? controller->State() : GameController::NoState;
    // Out of play here (loading, dying, restarting, a menu): forgotten, made again once play is back
    if (!InPlay(state))
    {
        ForgetPuppet();
        return;
    }

    InstanceContext* player = PlayerInstance();
    PlayerState partner;
    bool show = g_Session.Connected() && g_Session.Partner(now, &partner) && player != nullptr && player->chunk != nullptr &&
                state == GameController::StatePlaying && partner.area == local.area && partner.area != 0 &&
                !partner.Has(PlayerState::InBackpack) && !partner.Has(PlayerState::Watching) &&
                !partner.Has(PlayerState::Loading);
    if (!show)
    {
        HidePuppet();
        return;
    }

    // The partner's model found here by what it is, else this player's own (the same character, most of the time)
    ModelNode* own = ModelOf(player);
    GameOGI* ogi = OgiBySignature(partner.modelSignature);
    if (ogi == nullptr)
    {
        ogi = own != nullptr ? own->ogi : nullptr;
    }

    if (ogi == nullptr)
    {
        HidePuppet();
        return;
    }

    // Made again when it's gone or this player is in another chunk (the old one left hidden: its chunk frees it)
    if (!PuppetValid() || g_Puppet.chunk != player->chunk)
    {
        ForgetPuppet();
        g_Puppet.instance = MakePuppet(player->chunk, ogi);
        g_Puppet.node = ModelOf(g_Puppet.instance);
        g_Puppet.chunk = player->chunk;
        g_Puppet.signature = partner.modelSignature;
        Native::Log("coop: the partner's puppet made in %s (%s model, %u joints)",
                    player->chunk->name.string != nullptr ? player->chunk->name.string : "?",
                    OgiBySignature(partner.modelSignature) != nullptr ? "their" : "this player's",
                    static_cast<unsigned>(ogi->jointCount));
    }
    else if (g_Puppet.node->ogi != ogi)
    {
        // The partner switched character (Crash, Cortex, Nina), or the model was let go
        g_Puppet.node->SetOgi(ogi, 0, 0);
        g_Puppet.signature = partner.modelSignature;
    }

    // $TWIN_COOP_LOG_SMOOTH (tests): how evenly the puppet moves, frame by frame, logged every 3 s while the partner moves: frames
    // it stood still (stutter), frames it went more than twice the mean (jumps)
    static const bool logSmooth = std::getenv("TWIN_COOP_LOG_SMOOTH") != nullptr;
    if (logSmooth)
    {
        static float lastX = 0, lastZ = 0;
        static std::vector<float> steps;
        static double since = now;
        float step = std::sqrt((partner.position[0] - lastX) * (partner.position[0] - lastX) +
                               (partner.position[2] - lastZ) * (partner.position[2] - lastZ));
        lastX = partner.position[0];
        lastZ = partner.position[2];
        if (step < 5.0f)
        {
            steps.push_back(step);
        }

        if (now - since >= 3.0)
        {
            float sum = 0;
            for (float value : steps)
            {
                sum += value;
            }

            float mean = steps.empty() ? 0.0f : sum / static_cast<float>(steps.size());
            int still = 0;
            int jumps = 0;
            for (float value : steps)
            {
                still += mean > 0.01f && value < mean * 0.1f ? 1 : 0;
                jumps += mean > 0.01f && value > mean * 2.0f ? 1 : 0;
            }

            Native::Log("coop: smoothness: %zu frames, mean step %.3f, %d still, %d jumps", steps.size(), mean, still, jumps);
            steps.clear();
            since = now;
        }
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

// A joint of a pose as it travels: its turn (a unit quaternion), translation and rows' lengths
struct JointPose
{
    float q[4];
    float t[3];
    float s[3];
};

// A packed pose read back: false when it isn't for that many joints
bool DecodePose(const std::vector<uint8_t>& pose, u32 count, JointPose* joints)
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
        JointPose& out = joints[joint];
        float length = 0;
        for (float& part : out.q)
        {
            part = static_cast<float>(static_cast<s16>(reader.U16())) / 32767.0f;
            length += part * part;
        }

        length = std::sqrt(length);
        for (float& part : out.q)
        {
            part = length > 0 ? part / length : 0.0f;
        }

        for (float& value : out.t)
        {
            value = static_cast<float>(static_cast<s16>(reader.U16())) * translationScale;
        }

        for (float& value : out.s)
        {
            value = static_cast<float>(reader.U8()) / 64.0f;
        }
    }

    return reader.Ok();
}

// A joint's matrix from its turn, translation and rows' lengths (the pair of PackPose's quaternion)
void ComposeJoint(const JointPose& joint, Matrix4x4* matrix)
{
    float x = joint.q[0], y = joint.q[1], z = joint.q[2], w = joint.q[3];
    float r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                     {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                     {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    for (int row = 0; row < 3; row++)
    {
        for (int column = 0; column < 3; column++)
        {
            matrix->m[row][column] = r[row][column] * joint.s[row];
        }

        matrix->m[row][3] = 0.0f;
        matrix->m[3][row] = joint.t[row];
    }

    matrix->m[3][3] = 1.0f;
}

// Two packed poses blended a share of the way from the first to the second, written over joints' matrices: each joint's turn the
// short way (normalised), its translation and lengths in a straight line. False (nothing written) when they aren't for that many
// joints
bool BlendPoses(const std::vector<uint8_t>& from, const std::vector<uint8_t>& to, float share, Matrix4x4* joints, u32 count)
{
    static std::vector<JointPose> a;
    static std::vector<JointPose> b;
    a.resize(count);
    b.resize(count);
    if (!DecodePose(from, count, a.data()))
    {
        return false;
    }

    bool blend = &from != &to && share > 0.0f && DecodePose(to, count, b.data());
    share = share > 1.0f ? 1.0f : share;
    for (u32 index = 0; index < count; index++)
    {
        JointPose joint = a[index];
        if (blend)
        {
            const JointPose& next = b[index];
            float dot = joint.q[0] * next.q[0] + joint.q[1] * next.q[1] + joint.q[2] * next.q[2] + joint.q[3] * next.q[3];
            float sign = dot < 0 ? -1.0f : 1.0f;
            float length = 0;
            for (int part = 0; part < 4; part++)
            {
                joint.q[part] += (next.q[part] * sign - joint.q[part]) * share;
                length += joint.q[part] * joint.q[part];
            }

            length = std::sqrt(length);
            for (float& part : joint.q)
            {
                part = length > 0 ? part / length : 0.0f;
            }

            for (int axis = 0; axis < 3; axis++)
            {
                joint.t[axis] += (next.t[axis] - joint.t[axis]) * share;
                joint.s[axis] += (next.s[axis] - joint.s[axis]) * share;
            }
        }

        ComposeJoint(joint, &joints[index]);
    }

    return true;
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

// The backpack as this game has it: the player's character hidden (ModelDrawn), its pad taken (kept to give back), its events off
// (no hits, no pickups: the partner's game has them) and carried where the partner is, its own camera following it
struct BackpackHold
{
    bool applied = false;
    InstanceContext* instance = nullptr;
    GamePad* pad = nullptr;
};
BackpackHold g_Backpack;

PlayerCharacter* CharacterOf(InstanceContext* instance)
{
    auto* node = instance != nullptr ? static_cast<PlayerNode*>(GetGameNode(&instance->nodes, NodeCharacter)) : nullptr;
    return node != nullptr ? node->character : nullptr;
}

ControlsNode* ControlsOf(InstanceContext* instance)
{
    return instance != nullptr ? static_cast<ControlsNode*>(GetGameNode(&instance->nodes, NodeControls)) : nullptr;
}

void ApplyBackpack(InstanceContext* player)
{
    ControlsNode* controls = ControlsOf(player);
    PlayerCharacter* character = CharacterOf(player);
    if (controls == nullptr || character == nullptr)
    {
        return;
    }

    g_Backpack.applied = true;
    g_Backpack.instance = player;
    g_Backpack.pad = controls->pad;
    controls->pad = nullptr;
    character->moveInput = {0.0f, 0.0f, 0.0f, 1.0f};
    character->eventFlags.eventsOff = 1;
    Native::Log("coop: into the partner's backpack");
}

void ReleaseBackpack()
{
    if (!g_Backpack.applied)
    {
        return;
    }

    // The character given back only when it's still the one played (a level load may have made another)
    InstanceContext* player = PlayerInstance();
    if (player == g_Backpack.instance && InstanceLive(player))
    {
        ControlsNode* controls = ControlsOf(player);
        PlayerCharacter* character = CharacterOf(player);
        if (controls != nullptr && controls->pad == nullptr)
        {
            controls->pad = g_Backpack.pad;
        }

        if (character != nullptr)
        {
            character->eventFlags.eventsOff = 0;
        }
    }

    g_Backpack = BackpackHold();
    Native::Log("coop: out of the backpack");
}

void RideInBackpack(double now)
{
    // Not while the partner is dying, loading or restarting (where they are then means nothing), nor in another area (each area
    // has its own space: the same numbers are another place)
    PlayerState partner;
    InstanceContext* player = PlayerInstance();
    if (!g_Session.Partner(now, &partner) || partner.Has(PlayerState::Loading) || player == nullptr ||
        partner.area != AreaOfPlayer(player))
    {
        return;
    }

    PutPlayerAt(partner.position[0], partner.position[1], partner.position[2]);
    PlayerCharacter* character = CharacterOf(PlayerInstance());
    if (character != nullptr)
    {
        character->moveInput = {0.0f, 0.0f, 0.0f, 1.0f};
    }
}

// The world shared: what the players do to things, and what they collect. The kinds of the world events
enum WorldKind : uint8_t
{
    WorldAttack = 1,      // the player attacked something (a crate, a statue, a creature...): value the attack's kind, and the
                          // thing's node kind (Shift) above it
    WorldWumpaPickup = 2, // wumpa fruit picked up from a pickup (value: how many; the pickup hidden on the other PC)
    WorldWumpa = 3,       // wumpa fruit from anything else (a crate's own count): value
    WorldGem = 4,         // a gem found: value its colour, PickupFlag when it came from a pickup (hidden on the other PC)
    WorldCrystal = 5,     // the level's crystal found: value PickupFlag when it came from a pickup
};
constexpr u32 AttackKindShift = 8;
constexpr u32 PickupFlag = 0x100;

// The agents whose attacks are shared (the playable characters, pickups and projectiles aren't: the characters are each game's own,
// the pickups go by what's collected, the projectiles are gone at once)
constexpr NodeKind SharedAgentKinds[] = {NodeCrate, NodeGenericObject, NodeCreature, NodeGrabbable, NodePayGate};

// The shared agent kind an instance has (NodeMovement: none)
NodeKind SharedKindOf(InstanceContext* instance)
{
    for (NodeKind kind : SharedAgentKinds)
    {
        if (GetGameNode(&instance->nodes, kind) != nullptr)
        {
            return kind;
        }
    }

    return NodeMovement;
}

// How far a thing of a kind may be from where the partner's game had it: what stays put closely, creatures (each game moves its own)
// further
float ReachOf(NodeKind kind)
{
    return kind == NodeCreature || kind == NodeGrabbable ? 4.0f : 1.0f;
}

// The crates attacked from the partner's game: the attacks queued here that haven't arrived (not told back), and until when their
// contents and wumpa fruit are left out
struct Replayed
{
    u32 queued = 0;
    double queuedAt = 0;
    double until = 0;
};
std::map<const InstanceContext*, Replayed> g_Replayed;
constexpr double ReplayedSeconds = 15;

u32 AreaOf(const InstanceContext* instance)
{
    return instance != nullptr && instance->chunk != nullptr && instance->chunk->name.string != nullptr
               ? HashKey(instance->chunk->name.string)
               : 0;
}

Vector4 PositionOfInstance(InstanceContext* instance)
{
    ObjectPlace* place = instance->place;
    place->SyncPosition();
    return place->position;
}

// The instances of some node kinds in a chunk (as the game's own queries ask: none asleep)
s32 QueryKinds(ChunkData* chunk, u32 kinds, void** results, u16 most)
{
    InstanceQuery query = {};
    query.results = results;
    query.most = most;
    query.distance = 1e30f;
    query.bits.value = InstanceQueryBits::AllWanted;
    query.unwantedFlags = ReferencedObjectFlags::Asleep;
    return QueryChunkInstancesOfKinds(chunk, kinds, &query);
}

// The instance of a kind in the player's chunk the partner's event names: the one with its ID, else the nearest within reach
InstanceContext* FindInstance(const WorldEvent& event, NodeKind kind, float reach = 1.0f)
{
    InstanceContext* player = PlayerInstance();
    if (player == nullptr || player->chunk == nullptr || AreaOf(player) != event.area)
    {
        return nullptr;
    }

    constexpr u16 Most = 0x100;
    void* results[Most];
    s32 count = QueryKinds(player->chunk, 1u << kind, results, Most);
    InstanceContext* nearest = nullptr;
    float best = reach * reach;
    for (s32 index = 0; index < count; index++)
    {
        auto* instance = static_cast<InstanceContext*>(results[index]);
        Vector4 at = PositionOfInstance(instance);
        float dx = at.x - event.position[0];
        float dy = at.y - event.position[1];
        float dz = at.z - event.position[2];
        float distance = dx * dx + dy * dy + dz * dz;
        if (event.id != static_cast<u32>(InstanceContext::NoId) && static_cast<u32>(instance->id) == event.id &&
            distance < 25.0f)
        {
            return instance;
        }

        if (distance < best)
        {
            best = distance;
            nearest = instance;
        }
    }

    return nearest;
}

WorldEvent EventAbout(WorldKind kind, InstanceContext* instance, u32 value)
{
    WorldEvent event;
    event.kind = kind;
    event.value = value;
    event.area = AreaOf(instance);
    event.id = instance != nullptr ? static_cast<u32>(instance->id) : static_cast<u32>(InstanceContext::NoId);
    if (instance != nullptr)
    {
        Vector4 at = PositionOfInstance(instance);
        event.position[0] = at.x;
        event.position[1] = at.y;
        event.position[2] = at.z;
    }

    return event;
}

void WatchCrate(InstanceContext* crate, double now, const char* why);

// The partner's crate attacks waiting for this game's player to come near (the game runs the scripts of what's near the player
// only: a crate attacked far away takes it, but never breaks)
struct WaitingAttack
{
    WorldEvent event;
    double until;
};
std::vector<WaitingAttack> g_WaitingAttacks;
constexpr float NearCrateReach = 15.0f;
constexpr double WaitingAttackSeconds = 15 * 60;

bool PlayerNear(const WorldEvent& event)
{
    InstanceContext* player = PlayerInstance();
    if (player == nullptr || player->place == nullptr || AreaOf(player) != event.area)
    {
        return false;
    }

    Vector4 at = PositionOfInstance(player);
    float dx = at.x - event.position[0];
    float dy = at.y - event.position[1];
    float dz = at.z - event.position[2];
    return dx * dx + dy * dy + dz * dz <= NearCrateReach * NearCrateReach;
}

// The partner's world events made this game's
void ApplyWorld(double now)
{
    // The waiting attacks whose crates this player has come near: taken as they had just come
    std::vector<WorldEvent> due;
    for (auto waiting = g_WaitingAttacks.begin(); waiting != g_WaitingAttacks.end();)
    {
        if (PlayerNear(waiting->event))
        {
            due.push_back(waiting->event);
            waiting = g_WaitingAttacks.erase(waiting);
        }
        else
        {
            waiting = now > waiting->until ? g_WaitingAttacks.erase(waiting) : std::next(waiting);
        }
    }

    WorldEvent event;
    size_t dueIndex = 0;
    while (dueIndex < due.size() || g_Session.PopWorld(&event))
    {
        if (dueIndex < due.size())
        {
            event = due[dueIndex++];
        }

        switch (event.kind)
        {
        case WorldAttack:
        {
            auto kind = static_cast<NodeKind>(event.value >> AttackKindShift);
            u32 attack = event.value & ((1u << AttackKindShift) - 1);
            if (!PlayerNear(event))
            {
                // Kept once (the same thing's same attack again adds nothing)
                bool kept = false;
                for (const WaitingAttack& waiting : g_WaitingAttacks)
                {
                    kept = kept || (waiting.event.value == event.value && waiting.event.area == event.area &&
                                    std::fabs(waiting.event.position[0] - event.position[0]) < 0.1f &&
                                    std::fabs(waiting.event.position[2] - event.position[2]) < 0.1f);
                }

                if (!kept)
                {
                    g_WaitingAttacks.push_back({event, now + WaitingAttackSeconds});
                    Native::Log("coop: the partner's attack at (%.1f, %.1f, %.1f) is far from here: done when this player comes near",
                                event.position[0], event.position[1], event.position[2]);
                }

                break;
            }

            InstanceContext* target = FindInstance(event, kind, ReachOf(kind));
            if (target == nullptr)
            {
                Native::Log("coop: the partner's attack (node kind %u) at (%.1f, %.1f, %.1f): nothing found here", kind,
                            event.position[0], event.position[1], event.position[2]);
                break;
            }

            // Attacked the way the partner's character attacked it, by the partner's puppet (where the partner is: scripts look at
            // who attacks from where), else by this game's player
            Replayed& replayed = g_Replayed[target];
            replayed.queued++;
            replayed.queuedAt = now;
            replayed.until = now + ReplayedSeconds;
            InstanceContext* attacker = PlayerInstance();
            if (PuppetValid() && g_Puppet.chunk == target->chunk)
            {
                attacker = g_Puppet.instance;
            }

            QueueAttack(target, attack, attacker, 1u << kind);
            if (kind == NodeCrate)
            {
                WatchCrate(target, now, "the partner's attack");
            }

            break;
        }
        case WorldGem:
        case WorldCrystal:
        {
            // Found here too (the same progress mark and HUD as the partner's pickup), the pickup hidden
            if (event.kind == WorldGem)
            {
                AwardGem(static_cast<int>(event.value & 0xFF));
            }
            else
            {
                AwardCrystal();
            }

            InstanceContext* pickup = (event.value & PickupFlag) != 0 ? FindInstance(event, NodePickup, 1.5f) : nullptr;
            if (pickup != nullptr)
            {
                CallVirtual<u32>(pickup, pickup->vtable, ReferencedObject::SleepSlot);
            }

            Native::Log("coop: the partner found %s%s", event.kind == WorldGem ? "a gem" : "the crystal",
                        pickup != nullptr ? " (its pickup hidden here)" : "");
            break;
        }
        case WorldWumpaPickup:
        case WorldWumpa:
        {
            GameController* controller = G_GameController;
            if (controller != nullptr)
            {
                controller->oleg.wumpaToAdd = static_cast<u8>(controller->oleg.wumpaToAdd + event.value);
            }

            // The pickup the partner took: put to sleep here (out of sight and out of reach)
            InstanceContext* pickup = event.kind == WorldWumpaPickup ? FindInstance(event, NodePickup, 1.0f) : nullptr;
            if (pickup != nullptr)
            {
                CallVirtual<u32>(pickup, pickup->vtable, ReferencedObject::SleepSlot);
            }

            Native::Log("coop: %u wumpa fruit from the partner%s", event.value,
                        event.kind != WorldWumpaPickup ? "" : (pickup != nullptr ? " (their pickup hidden here)" : " (their pickup not found here)"));

            break;
        }
        default:
            break;
        }
    }

    // Forgotten once past their time
    for (auto entry = g_Replayed.begin(); entry != g_Replayed.end();)
    {
        entry = now > entry->second.until ? g_Replayed.erase(entry) : std::next(entry);
    }
}

// Crates looked at again a while after an attack (the tests' log): whether they're still there, and the wumpa count
struct CrateWatch
{
    InstanceContext* crate;
    double at;
    const char* why;
};
std::vector<CrateWatch> g_CrateWatches;

void WatchCrate(InstanceContext* crate, double now, const char* why)
{
    if (std::getenv("TWIN_COOP_TEST_CRATE") != nullptr || std::getenv("TWIN_COOP_LOG_CRATES") != nullptr)
    {
        g_CrateWatches.push_back({crate, now + 3.0, why});
    }
}

void CrateWatchFrame(double now)
{
    GameController* controller = G_GameController;
    for (auto watch = g_CrateWatches.begin(); watch != g_CrateWatches.end();)
    {
        if (now < watch->at)
        {
            ++watch;
            continue;
        }

        bool live = InstanceLive(watch->crate);
        Native::Log("coop: 3 s after %s, the crate is %s (asleep %u, visible %u); wumpa %u (+%u to add)", watch->why,
                    live ? "still there" : "gone", live ? static_cast<unsigned>(watch->crate->flags.asleep) : 1u,
                    live ? static_cast<unsigned>(watch->crate->flags.visible) : 0u,
                    controller != nullptr ? static_cast<unsigned>(controller->progress.counts.wumpa) : 0u,
                    controller != nullptr ? static_cast<unsigned>(controller->oleg.wumpaToAdd) : 0u);
        watch = g_CrateWatches.erase(watch);
    }
}

bool IsReplayed(const InstanceContext* instance)
{
    auto found = g_Replayed.find(instance);
    return found != g_Replayed.end() && Now() <= found->second.until;
}

// $TWIN_COOP_TEST_CRATE=seconds (tests): that long after connecting, the player is put beside the nearest crate that takes hits
// (within 30 units) and spins at it a second later
void TestCrate(double now, double connectedAt)
{
    static const char* setting = std::getenv("TWIN_COOP_TEST_CRATE");
    static int stage = 0;
    static InstanceContext* target = nullptr;
    static double spinAt = 0;
    InstanceContext* player = PlayerInstance();
    if (setting == nullptr || stage == 2 || connectedAt < 0 || now - connectedAt < std::atof(setting) || player == nullptr ||
        player->chunk == nullptr)
    {
        return;
    }

    if (stage == 1)
    {
        if (now >= spinAt)
        {
            stage = 2;
            if (target != nullptr && InstanceLive(target))
            {
                Vector4 at = PositionOfInstance(target);
                Native::Log("coop: test: spinning at the crate at (%.1f, %.1f, %.1f)", at.x, at.y, at.z);
                QueueAttack(target, AttackSpin, player, 1u << NodeCrate);
                WatchCrate(target, now, "the test's spin");
            }
        }

        return;
    }

    stage = 1;
    spinAt = now + 1.0;
    constexpr u16 Most = 0x100;
    void* results[Most];
    s32 count = QueryKinds(player->chunk, 1u << NodeCrate, results, Most);
    Vector4 from = PositionOfInstance(player);
    InstanceContext* nearest = nullptr;
    float best = 30.0f * 30.0f;
    for (s32 index = 0; index < count; index++)
    {
        auto* crate = static_cast<InstanceContext*>(results[index]);
        Vector4 at = PositionOfInstance(crate);
        float distance = (at.x - from.x) * (at.x - from.x) + (at.y - from.y) * (at.y - from.y) + (at.z - from.z) * (at.z - from.z);
        // Only a crate that takes hits (the flat ones laid on the ground don't)
        if (distance < best && crate->flags.receivesTriggerSignals != 0)
        {
            best = distance;
            nearest = crate;
        }
    }

    if (nearest != nullptr)
    {
        Vector4 at = PositionOfInstance(nearest);
        Native::Log("coop: test: going beside the crate at (%.1f, %.1f, %.1f), %d crates here (id %d)", at.x, at.y, at.z, count,
                    nearest->id);
        target = nearest;
        PutPlayerAt(at.x + 1.2f, at.y, at.z);
    }
    else
    {
        stage = 2;
        Native::Log("coop: test: no crate within 30 units (%d here)", count);
    }
}

// $TWIN_COOP_TEST_PICKUP=seconds (tests): that long after connecting, the player is put on the nearest pickup within 40 units (the
// game picks it up as it would); the wumpa count logged 3 s later here and on the partner's PC
void TestPickup(double now, double connectedAt)
{
    static const char* setting = std::getenv("TWIN_COOP_TEST_PICKUP");
    static bool done = false;
    static double logAt = -1;
    GameController* controller = G_GameController;
    if (logAt >= 0 && now >= logAt)
    {
        logAt = -1;
        Native::Log("coop: test: 3 s after the pickup, wumpa %u (+%u to add)",
                    controller != nullptr ? static_cast<unsigned>(controller->progress.counts.wumpa) : 0u,
                    controller != nullptr ? static_cast<unsigned>(controller->oleg.wumpaToAdd) : 0u);
    }

    InstanceContext* player = PlayerInstance();
    if (setting == nullptr || done || connectedAt < 0 || now - connectedAt < std::atof(setting) || player == nullptr ||
        player->chunk == nullptr)
    {
        return;
    }

    done = true;
    constexpr u16 Most = 0x100;
    void* results[Most];
    s32 count = QueryKinds(player->chunk, 1u << NodePickup, results, Most);
    Vector4 from = PositionOfInstance(player);
    InstanceContext* nearest = nullptr;
    float best = 40.0f * 40.0f;
    for (s32 index = 0; index < count; index++)
    {
        auto* pickup = static_cast<InstanceContext*>(results[index]);
        Vector4 at = PositionOfInstance(pickup);
        float distance = (at.x - from.x) * (at.x - from.x) + (at.y - from.y) * (at.y - from.y) + (at.z - from.z) * (at.z - from.z);
        if (distance < best && pickup->flags.visible != 0)
        {
            best = distance;
            nearest = pickup;
        }
    }

    if (nearest == nullptr)
    {
        Native::Log("coop: test: no pickup within 40 units (%d here)", count);
        return;
    }

    Vector4 at = PositionOfInstance(nearest);
    Native::Log("coop: test: going onto the pickup at (%.1f, %.1f, %.1f), %d pickups here; wumpa %u", at.x, at.y, at.z, count,
                controller != nullptr ? static_cast<unsigned>(controller->progress.counts.wumpa) : 0u);
    PutPlayerAt(at.x, at.y, at.z);
    logAt = now + 3.0;
}

// The backpack's state in the session made this game's: applied or released as it changes, kept up while in it
void BackpackFrame(double now)
{
    InstanceContext* player = PlayerInstance();
    GameController* controller = G_GameController;
    bool playing = controller != nullptr && controller->State() == GameController::StatePlaying;
    bool wanted = g_Session.InBackpack() && player != nullptr;
    // A new character to play (a level loaded, a switch): the hold made again on it
    if (g_Backpack.applied && (player != g_Backpack.instance || !wanted))
    {
        ReleaseBackpack();
    }

    if (wanted && !g_Backpack.applied && playing)
    {
        ApplyBackpack(player);
    }

    if (g_Backpack.applied && playing)
    {
        RideInBackpack(now);
    }
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

extern double g_ConnectedAt;
bool TestDue(const char* variable, int part, bool* done);

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

    // $TWIN_COOP_TEST_BACKPACK="in,out" (tests): into the backpack that many seconds after connecting, out again at the second
    static const char* testBackpack = std::getenv("TWIN_COOP_TEST_BACKPACK");
    static double connectedAt = -1;
    connectedAt = g_Session.Connected() ? (connectedAt < 0 ? now : connectedAt) : -1;
    if (testBackpack != nullptr && connectedAt >= 0)
    {
        static int stage = 0;
        double inAt = std::atof(testBackpack);
        const char* comma = std::strchr(testBackpack, ',');
        double outAt = comma != nullptr ? std::atof(comma + 1) : 1e9;
        if (stage == 0 && now - connectedAt >= inAt && g_Session.EnterBackpack())
        {
            stage = 1;
        }
        else if (stage == 1 && now - connectedAt >= outAt)
        {
            g_Session.LeaveBackpack();
            LeaveBesidePartner(now);
            stage = 2;
        }
    }

    // The game's state changing: a death (or a checkpoint's restart) told the partner; the pause told, or taken for the partner's
    GameController* controller = G_GameController;
    u32 state = controller != nullptr ? controller->State() : GameController::NoState;
    static u32 lastState = GameController::NoState;
    if (state != lastState)
    {
        if (state == GameController::StateFadingOut && InPlay(lastState))
        {
            g_Session.SendDied();
            Native::Log("coop: back to the checkpoint (told the partner)");
        }

        bool pausedNow = state == GameController::StatePaused || state == GameController::StateGallery;
        bool pausedBefore = lastState == GameController::StatePaused || lastState == GameController::StateGallery;
        if (pausedNow && !pausedBefore)
        {
            g_PausedBy = g_PausingForPartner ? PausedBy::Partner : PausedBy::Me;
            if (g_PausedBy == PausedBy::Me)
            {
                g_Session.SendPause();
            }

            Native::Log("coop: paused (%s)", g_PausedBy == PausedBy::Me ? "by this player" : "by the partner");
        }
        else if (!pausedNow && pausedBefore)
        {
            if (g_PausedBy == PausedBy::Me)
            {
                g_Session.SendResume();
            }

            g_PausedBy = PausedBy::Nobody;
            Native::Log("coop: resumed");
        }

        g_PausingForPartner = false;
        lastState = state;
    }

    // $TWIN_COOP_TEST_DIE=seconds (tests): back to the checkpoint that long after connecting, as a death does
    g_ConnectedAt = connectedAt;
    static bool testDied = false;
    if (controller != nullptr && state == GameController::StatePlaying && TestDue("TWIN_COOP_TEST_DIE", 0, &testDied))
    {
        Native::Log("coop: test: dying");
        controller->RestartFromCheckpoint();
    }

    // The carrier went back to their checkpoint: the backpack's rider goes back to theirs too (the two come back together)
    if (g_Session.TakeDied() && g_Backpack.applied && controller != nullptr && InPlay(state))
    {
        controller->RestartFromCheckpoint();
        g_Session.Notify("Co-op: " + g_Session.PartnerName() + " morreu: voltando ao checkpoint junto", 4);
    }

    ApplyWorld(now);
    TestCrate(now, connectedAt);
    TestPickup(now, connectedAt);
    CrateWatchFrame(now);
    BackpackFrame(now);
    UpdatePuppet(now, local);

    // The HUD: a session that couldn't start first, then the cutscene's wait, else the session's latest notice (connected, left...)
    std::string line = RetryStart(now);
    if (line.empty())
    {
        line = GateLine(now);
    }

    if (line.empty() && !g_Session.Notices().empty())
    {
        line = g_Session.Notices().back().text;
    }

    ShowLine(line);
}

void AgentAttacked(InstanceContext* instance, unsigned kind, InstanceContext* sender)
{
    if (!g_Session.Connected() || instance == nullptr)
    {
        return;
    }

    NodeKind shared = SharedKindOf(instance);
    if (shared == NodeMovement)
    {
        return;
    }

    // An attack this game queued for the partner's: arrived, not told back
    auto found = g_Replayed.find(instance);
    if (found != g_Replayed.end() && found->second.queued != 0)
    {
        found->second.queued--;
        return;
    }

    // Only the player's own, on things that take hits (the flat crates laid on the ground and the like don't)
    if (sender == nullptr || sender != PlayerInstance() || instance->flags.receivesTriggerSignals == 0)
    {
        return;
    }

    // The same thing's same attack again within a moment (standing on it, a spin's several touches): once
    static std::map<std::pair<const InstanceContext*, unsigned>, double> lastSent;
    double now = Now();
    double& last = lastSent[{instance, kind}];
    if (now - last < 0.3)
    {
        return;
    }

    last = now;
    if (lastSent.size() > 256)
    {
        lastSent.clear();
    }

    Vector4 at = PositionOfInstance(instance);
    Native::Log("coop: the player's attack %u on a thing of node kind %u at (%.1f, %.1f, %.1f): told the partner", kind, shared,
                at.x, at.y, at.z);
    g_Session.SendWorld(EventAbout(WorldAttack, instance, kind | (static_cast<u32>(shared) << AttackKindShift)));
}

// A gem or the crystal found by this player (commandsgame.cpp's AddGem and AddCrystal): told the partner, unless it's the partner's
// own being made here (AwardGem, AwardCrystal)
bool g_Awarding = false;

void CollectibleFound(bool gem, int colour, InstanceContext* from)
{
    if (!g_Session.Connected() || g_Awarding)
    {
        return;
    }

    bool pickup = from != nullptr && GetGameNode(&from->nodes, NodePickup) != nullptr;
    u32 value = (gem ? static_cast<u32>(colour) & 0xFF : 0) | (pickup ? PickupFlag : 0);
    g_Session.SendWorld(EventAbout(gem ? WorldGem : WorldCrystal, from, value));
    Native::Log("coop: %s found: told the partner", gem ? "a gem" : "the crystal");
}

// Bodies held back for a cutscene's wait (behaviours.cpp), by their level
struct HeldEntry
{
    const GraphState* state;
    StateBody* body;
    int way;
};
std::map<const BehaviourLevel*, HeldEntry> g_Held;

StateBody* HeldBody(const BehaviourLevel* level, const GraphState* state, int* way)
{
    auto found = g_Held.find(level);
    if (found == g_Held.end())
    {
        return nullptr;
    }

    // The level moved on (another state, a level unwound): forgotten
    if (found->second.state != state)
    {
        g_Held.erase(found);
        return nullptr;
    }

    *way = found->second.way;
    return found->second.body;
}

void HoldBody(const BehaviourLevel* level, const GraphState* state, StateBody* body, int way)
{
    g_Held[level] = {state, body, way};
}

void ReleaseHeldBody(const BehaviourLevel* level)
{
    g_Held.erase(level);
}

bool SuppressesCrateContents(const InstanceContext* crate)
{
    bool suppressed = g_Session.GetRole() != Role::Off && IsReplayed(crate);
    if (suppressed)
    {
        Native::Log("coop: a crate the partner broke: its contents left to the partner's game");
    }

    return suppressed;
}

bool WumpaPicked(InstanceContext* from, unsigned count)
{
    if (g_Session.GetRole() == Role::Off || from == nullptr)
    {
        return false;
    }

    // From a crate the partner broke: the partner's game counted them (and told this one)
    if (GetGameNode(&from->nodes, NodePickup) == nullptr && IsReplayed(from))
    {
        return true;
    }

    bool pickup = GetGameNode(&from->nodes, NodePickup) != nullptr;
    g_Session.SendWorld(EventAbout(pickup ? WorldWumpaPickup : WorldWumpa, from, count));
    return false;
}

// $TWIN_COOP_TEST_PAUSE="pause,resume" (tests): this player pauses that many seconds after connecting, and resumes at the second
double TestSeconds(const char* variable, int part)
{
    const char* setting = std::getenv(variable);
    if (setting == nullptr)
    {
        return -1;
    }

    for (int at = 0; at < part && setting != nullptr; at++)
    {
        setting = std::strchr(setting, ',');
        setting = setting != nullptr ? setting + 1 : nullptr;
    }

    return setting != nullptr ? std::atof(setting) : -1;
}

double g_ConnectedAt = -1;

bool TestDue(const char* variable, int part, bool* done)
{
    double at = TestSeconds(variable, part);
    if (*done || at < 0 || g_ConnectedAt < 0 || Now() - g_ConnectedAt < at)
    {
        return false;
    }

    *done = true;
    return true;
}

bool TakePartnerPause()
{
    static bool testPaused = false;
    if (TestDue("TWIN_COOP_TEST_PAUSE", 0, &testPaused))
    {
        Native::Log("coop: test: pausing");
        return true;
    }

    if (!g_Session.Connected() || !g_Session.TakePause())
    {
        return false;
    }

    g_PausingForPartner = true;
    return true;
}

bool TakePartnerResume()
{
    static bool testResumed = false;
    if (g_PausedBy == PausedBy::Me && TestDue("TWIN_COOP_TEST_PAUSE", 1, &testResumed))
    {
        Native::Log("coop: test: resuming");
        return true;
    }

    // A resume for a pause that wasn't the partner's (both paused at once): this player's own stands
    bool resume = g_Session.TakeResume();
    return resume && g_PausedBy == PausedBy::Partner;
}

bool BlocksResume()
{
    if (!g_Session.Connected() || g_PausedBy != PausedBy::Partner)
    {
        return false;
    }

    g_Session.Notify("So " + g_Session.PartnerName() + " pode despausar", 3);
    return true;
}

bool HidesShadow(const InstanceContext* instance)
{
    if (RidingHidesShadow(instance))
    {
        return true;
    }

    return g_Backpack.applied && instance == g_Backpack.instance;
}

void TestStick(unsigned char* x, unsigned char* y)
{
    static const char* setting = std::getenv("TWIN_COOP_TEST_RUN");
    static double connectedAt = -1;
    if (setting == nullptr || g_Session.GetRole() == Role::Off)
    {
        return;
    }

    double now = Now();
    connectedAt = g_Session.Connected() ? (connectedAt < 0 ? now : connectedAt) : -1;
    if (connectedAt < 0 || now - connectedAt < std::atof(setting))
    {
        return;
    }

    // A turn every 6 seconds; a second standing still every 4 (the idle animation too)
    double running = now - connectedAt;
    if (std::fmod(running, 4.0) < 1.0)
    {
        return;
    }

    double angle = running * (2.0 * 3.14159265 / 6.0);
    *x = static_cast<unsigned char>(128.0 + 127.0 * std::cos(angle));
    *y = static_cast<unsigned char>(128.0 + 127.0 * std::sin(angle));
}

namespace
{
// The free camera's setting: $TWIN_CAMERA or coop.ini's camera ("free", the default, or "original"), then F7
bool& FreeCameraSetting()
{
    static bool made = false;
    static bool on = true;
    if (!made)
    {
        made = true;
        auto ini = ReadIni();
        on = Setting(ini, "camera", "TWIN_CAMERA", "free") != "original";
    }

    return on;
}
}

bool FreeCamera()
{
    return FreeCameraSetting();
}

void CameraKeys()
{
    static bool down = false;
    const bool* keys = SDL_GetKeyboardState(nullptr);
    bool now = keys != nullptr && keys[SDL_SCANCODE_F7];
    if (now && !down)
    {
        bool& on = FreeCameraSetting();
        on = !on;
        Native::Log("camera: %s", on ? "free (F7)" : "the game's own (F7)");
        g_Session.Notify(on ? "Camera livre (F7)" : "Camera original (F7)", 2.5);
    }

    down = now;
}

bool Active()
{
    StartOnce();
    return g_Session.GetRole() != Role::Off;
}

bool ModelDrawn(ModelNode* node)
{
    if (RidingHides(node))
    {
        return true;
    }

    if (g_Session.GetRole() == Role::Off || node == nullptr)
    {
        return false;
    }

    // This player's character: its pose taken for the next state packet; not drawn while it rides in the backpack
    InstanceContext* player = PlayerInstance();
    if (player != nullptr && node == ModelOf(player))
    {
        if (g_Backpack.applied)
        {
            return true;
        }

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

        return false;
    }

    // The partner's puppet: posed as the partner's character, when the pose is for its model's joints
    if (node == g_Puppet.node && PuppetValid())
    {
        u32 count = JointsWritten(node->drawnAnimator, node->drawnOgi);
        // The two poses around the moment the puppet is placed at, blended (the same moment as its place: Session::Partner)
        const std::vector<uint8_t>* from = nullptr;
        const std::vector<uint8_t>* to = nullptr;
        float share = 0;
        if (count != 0 && g_Session.PartnerPoses(Now(), &from, &to, &share))
        {
            BlendPoses(*from, *to, share, node->drawnAnimator->matrices.start, count);
        }
    }

    return false;
}

bool HoldsBody(const StateBody* body, BehaviourRunner* runner, BehaviourLevel* level)
{
    if (g_Session.GetRole() == Role::Off || body == nullptr)
    {
        return false;
    }

    // Only cutscenes that start from play (the title's flyby and the menus' scenes go on as they do)
    GameController* controller = G_GameController;
    if (controller == nullptr || controller->State() != GameController::StatePlaying)
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

    static uint32_t loggedHold = 0;
    if (loggedHold != key)
    {
        loggedHold = key;
        Native::Log("coop: cutscene %s (%08X) held: waiting for the partner", name.c_str(), key);
    }

    return true;
}
}
