// Online co-op's session (coopsession.h)
#include "coopsession.h"

#include <cmath>

namespace Coop
{
namespace
{
constexpr double HelloSeconds = 0.5;
constexpr double PingSeconds = 1.0;
// An ask not repeated this long: the script stopped waiting (the player walked off, or it started)
constexpr double AskLapseSeconds = 0.35;
// A Go kept for a cutscene the client hasn't reached yet
constexpr double GoKeepSeconds = 20;
constexpr size_t MostSamples = 64;

float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// Angles in radians, the short way round
float LerpAngle(float a, float b, float t)
{
    constexpr float Pi = 3.14159265f;
    float delta = std::fmod(b - a + 3 * Pi, 2 * Pi) - Pi;
    return a + delta * t;
}
}

uint32_t HashKey(const char* text, uint32_t seed)
{
    uint32_t hash = seed;
    for (; text != nullptr && *text != '\0'; text++)
    {
        hash = (hash ^ static_cast<uint8_t>(*text)) * 16777619u;
    }

    return hash != 0 ? hash : 1;
}

bool Session::Start(const Settings& wanted)
{
    Stop();
    settings = wanted;
    if (settings.role == Role::Off)
    {
        return true;
    }

    if (settings.role == Role::Client && !ParseAddress(settings.join, &hostAddress, settings.port))
    {
        error = "endereço do host inválido: \"" + settings.join + "\"";
        link = Link::Failed;
        return false;
    }

    if (!socket.Open(settings.role == Role::Host ? settings.port : 0))
    {
        error = socket.Error();
        link = Link::Failed;
        return false;
    }

    link = Link::Waiting;
    Notify(settings.role == Role::Host ? "Co-op: aguardando o Jogador 2 na porta " + std::to_string(socket.LocalPort())
                                       : "Co-op: conectando a " + hostAddress.ToString() + "...",
           6);
    return true;
}

void Session::Stop()
{
    if (link == Link::Connected)
    {
        std::vector<uint8_t> bye;
        Writer writer(&bye);
        WriteHeader(&writer, PacketKind::Bye);
        SendRaw(bye);
    }

    socket.Close();
    link = Link::Off;
    channel.Reset();
    samples.clear();
    haveLatest = false;
    latest = PlayerState();
    inBackpack = false;
    holdKey = partnerHoldKey = goKey = releasedKey = 0;
}

void Session::Notify(const std::string& text, double seconds)
{
    notices.push_back({text, now + seconds});
    while (notices.size() > 4)
    {
        notices.pop_front();
    }
}

void Session::SendRaw(const std::vector<uint8_t>& packet)
{
    if (peer.Valid())
    {
        socket.Send(peer, packet.data(), packet.size());
    }
}

void Session::Connect(const Address& from, double at)
{
    peer = from;
    link = Link::Connected;
    lastHeard = at;
    channel.Reset();
    samples.clear();
    haveLatest = false;
    partnerPose.clear();
    partnerPoseTick = 0;
    poses.clear();
    haveClockOffset = false;
    Notify("Co-op: " + (partnerName.empty() ? std::string("parceiro") : partnerName) + " conectou!", 5);
}

void Session::Disconnect(const std::string& why)
{
    if (link != Link::Connected)
    {
        return;
    }

    Notify("Co-op: " + why, 6);
    link = Link::Waiting;
    if (settings.role == Role::Host)
    {
        peer = Address();
    }

    channel.Reset();
    samples.clear();
    haveLatest = false;
    latest = PlayerState();
    partnerHoldKey = 0;
    partnerPose.clear();
    partnerPoseTick = 0;
    poses.clear();
    haveClockOffset = false;
    // Alone again: out of the backpack, and a held cutscene goes on solo
    if (inBackpack)
    {
        inBackpack = false;
    }
}

void Session::Update(double at, const PlayerState& local)
{
    now = at;
    while (!notices.empty() && notices.front().until < now)
    {
        notices.pop_front();
    }

    if (settings.role == Role::Off || !socket.IsOpen())
    {
        return;
    }

    Receive(now);

    if (link == Link::Connected && now - lastHeard > settings.timeoutSeconds)
    {
        Disconnect("conexão perdida com " + partnerName);
    }

    // The client says hello until welcomed
    if (settings.role == Role::Client && link == Link::Waiting && now - lastHello >= HelloSeconds)
    {
        lastHello = now;
        peer = hostAddress;
        std::vector<uint8_t> hello;
        Writer writer(&hello);
        WriteHeader(&writer, PacketKind::Hello);
        writer.Text(settings.name, 24);
        writer.U8(settings.skin);
        SendRaw(hello);
    }

    if (link != Link::Connected)
    {
        return;
    }

    // (A tenth of slack: a 60 Hz frame a little quicker than 1/60 s still sends, else every other frame would)
    if (now - lastState >= 0.9 / settings.stateHz)
    {
        lastState = now;
        SendState(local);
    }

    if (now - lastPing >= PingSeconds)
    {
        lastPing = now;
        std::vector<uint8_t> ping;
        Writer writer(&ping);
        WriteHeader(&writer, PacketKind::Ping);
        writer.F32(static_cast<float>(now));
        SendRaw(ping);
    }

    // A held cutscene the script stopped asking for: let go of
    if (holdKey != 0 && now - holdAsked > AskLapseSeconds)
    {
        SendEvent(EventKind::CutsceneCancel, holdKey);
        holdKey = 0;
    }

    std::vector<std::vector<uint8_t>> due;
    channel.Due(now, &due);
    for (const auto& packet : due)
    {
        SendRaw(packet);
    }
}

void Session::SendState(const PlayerState& local)
{
    PlayerState state = local;
    if (inBackpack)
    {
        state.flags |= PlayerState::InBackpack;
    }

    std::vector<uint8_t> packet;
    Writer writer(&packet);
    WriteHeader(&writer, PacketKind::State);
    writer.U32(++tick);
    // The sender's clock: the partner's game places the state on this PC's timeline by it (not by when it came: the network's
    // jitter would show as stutter)
    writer.F64(now);
    writer.U32(state.area);
    for (float value : state.position)
    {
        writer.F32(value);
    }

    writer.F32(state.yaw);
    for (float value : state.rotation)
    {
        writer.F32(value);
    }

    writer.U16(state.model);
    writer.U16(state.animation);
    writer.F32(state.animationTime);
    writer.U16(state.flags);
    writer.U8(state.skin);
    SendRaw(packet);

    // The pose in a packet of its own (it can be most of a datagram)
    if (!localPose.empty() && localPose.size() <= MostPacket - 16)
    {
        std::vector<uint8_t> pose;
        Writer poseWriter(&pose);
        WriteHeader(&poseWriter, PacketKind::Pose);
        poseWriter.U32(tick);
        poseWriter.F64(now);
        poseWriter.Bytes(localPose.data(), localPose.size());
        SendRaw(pose);
    }
}

void Session::Receive(double at)
{
    uint8_t buffer[MostPacket + 64];
    Address from;
    for (int got = socket.Receive(&from, buffer, sizeof(buffer)); got >= 0; got = socket.Receive(&from, buffer, sizeof(buffer)))
    {
        Handle(from, buffer, static_cast<size_t>(got), at);
    }
}

void Session::Handle(const Address& from, const uint8_t* data, size_t size, double at)
{
    Reader reader(data, size);
    PacketKind kind;
    if (!ReadHeader(&reader, &kind))
    {
        return;
    }

    // The host takes a hello from anyone while it has no client (and again from its client, whose welcome was lost)
    if (kind == PacketKind::Hello && settings.role == Role::Host)
    {
        if (link == Link::Connected && from != peer)
        {
            return;
        }

        std::string name = reader.Text();
        uint8_t skin = reader.U8();
        if (!reader.Ok())
        {
            return;
        }

        bool fresh = link != Link::Connected;
        partnerName = name.empty() ? "Jogador 2" : name;
        partnerSkin = skin;
        if (fresh)
        {
            Connect(from, at);
        }

        lastHeard = at;
        std::vector<uint8_t> welcome;
        Writer writer(&welcome);
        WriteHeader(&writer, PacketKind::Welcome);
        writer.Text(settings.name, 24);
        writer.U8(settings.skin);
        SendRaw(welcome);
        return;
    }

    if (kind == PacketKind::Welcome && settings.role == Role::Client)
    {
        if (from != hostAddress)
        {
            return;
        }

        std::string name = reader.Text();
        uint8_t skin = reader.U8();
        if (!reader.Ok())
        {
            return;
        }

        partnerName = name.empty() ? "Jogador 1" : name;
        partnerSkin = skin;
        if (link != Link::Connected)
        {
            Connect(from, at);
        }

        lastHeard = at;
        return;
    }

    if (link != Link::Connected || from != peer)
    {
        return;
    }

    lastHeard = at;
    switch (kind)
    {
    case PacketKind::State:
    {
        PlayerState state;
        state.tick = reader.U32();
        double sentAt = reader.F64();
        state.area = reader.U32();
        for (float& value : state.position)
        {
            value = reader.F32();
        }

        state.yaw = reader.F32();
        for (float& value : state.rotation)
        {
            value = reader.F32();
        }

        state.model = reader.U16();
        state.animation = reader.U16();
        state.animationTime = reader.F32();
        state.flags = reader.U16();
        state.skin = reader.U8();
        // Older than the newest (UDP reorders): dropped
        if (!reader.Ok() || (haveLatest && static_cast<int32_t>(state.tick - latest.tick) <= 0))
        {
            return;
        }

        latest = state;
        haveLatest = true;
        samples.push_back({PartnerTime(sentAt, at), state});
        while (samples.size() > MostSamples)
        {
            samples.pop_front();
        }

        return;
    }
    case PacketKind::Pose:
    {
        uint32_t poseTick = reader.U32();
        double sentAt = reader.F64();
        if (!reader.Ok() || (partnerPoseTick != 0 && static_cast<int32_t>(poseTick - partnerPoseTick) <= 0))
        {
            return;
        }

        partnerPoseTick = poseTick;
        partnerPose.assign(reader.Here(), reader.Here() + reader.Left());
        poses.push_back({PartnerTime(sentAt, at), partnerPose});
        while (poses.size() > MostSamples)
        {
            poses.pop_front();
        }

        return;
    }
    case PacketKind::Reliable:
    {
        uint16_t number = reader.U16();
        if (!reader.Ok())
        {
            return;
        }

        std::vector<uint8_t> ack;
        std::vector<std::vector<uint8_t>> delivered;
        channel.Received(number, reader.Here(), reader.Left(), &ack, &delivered);
        SendRaw(ack);
        for (const auto& message : delivered)
        {
            HandleEvent(message.data(), message.size(), at);
        }

        return;
    }
    case PacketKind::Ack:
    {
        uint16_t number = reader.U16();
        if (reader.Ok())
        {
            channel.Acknowledged(number);
        }

        return;
    }
    case PacketKind::Ping:
    {
        float sent = reader.F32();
        std::vector<uint8_t> pong;
        Writer writer(&pong);
        WriteHeader(&writer, PacketKind::Pong);
        writer.F32(sent);
        SendRaw(pong);
        return;
    }
    case PacketKind::Pong:
    {
        float sent = reader.F32();
        if (reader.Ok())
        {
            double measured = at - sent;
            roundTrip = roundTrip == 0 ? measured : roundTrip * 0.8 + measured * 0.2;
        }

        return;
    }
    case PacketKind::Bye:
        Disconnect(partnerName + " saiu do jogo");
        return;
    default:
        return;
    }
}

void Session::SendEvent(EventKind kind, uint32_t a, uint32_t b, uint32_t c)
{
    if (link != Link::Connected)
    {
        return;
    }

    std::vector<uint8_t> message;
    Writer writer(&message);
    writer.U8(static_cast<uint8_t>(kind));
    writer.U32(a);
    writer.U32(b);
    writer.U32(c);
    channel.Queue(message);
}

void Session::SendEventText(EventKind kind, const std::string& text)
{
    if (link != Link::Connected)
    {
        return;
    }

    std::vector<uint8_t> message;
    Writer writer(&message);
    writer.U8(static_cast<uint8_t>(kind));
    writer.Text(text, 200);
    channel.Queue(message);
}

void Session::HandleEvent(const uint8_t* data, size_t size, double at)
{
    Reader reader(data, size);
    auto kind = static_cast<EventKind>(reader.U8());
    if (kind == EventKind::World)
    {
        WorldEvent event;
        event.kind = reader.U8();
        event.area = reader.U32();
        event.id = reader.U32();
        event.value = reader.U32();
        for (float& value : event.position)
        {
            value = reader.F32();
        }

        if (reader.Ok())
        {
            world.push_back(event);
        }

        return;
    }

    if (kind == EventKind::Text)
    {
        std::string text = reader.Text();
        if (reader.Ok())
        {
            Notify(partnerName + ": " + text, 5);
        }

        return;
    }

    uint32_t a = reader.U32();
    uint32_t b = reader.U32();
    uint32_t c = reader.U32();
    if (!reader.Ok())
    {
        return;
    }

    switch (kind)
    {
    case EventKind::CutsceneHold:
        partnerHoldKey = a;
        if (holdKey != a)
        {
            Notify(partnerName + " está esperando você para uma cutscene", 5);
        }

        break;
    case EventKind::CutsceneCancel:
        if (partnerHoldKey == a)
        {
            partnerHoldKey = 0;
        }

        break;
    case EventKind::CutsceneGo:
        // The host started it: let the script go if it waits there, else keep the Go for when it does
        if (holdKey == a)
        {
            ReleaseGate(a);
        }
        else
        {
            goKey = a;
            goAt = at;
        }

        break;
    case EventKind::BackpackEnter:
        // Both at once: the host keeps the backpack
        if (inBackpack)
        {
            if (settings.role == Role::Host)
            {
                SendEvent(EventKind::BackpackDenied);
                break;
            }

            inBackpack = false;
            Notify("Co-op: " + partnerName + " já estava entrando na mochila", 3);
        }

        Notify(partnerName + " entrou na sua mochila", 3);
        break;
    case EventKind::BackpackLeave:
        Notify(partnerName + " saiu da mochila", 3);
        break;
    case EventKind::BackpackDenied:
        inBackpack = false;
        Notify("Co-op: não dá para entrar na mochila agora", 3);
        break;
    case EventKind::Pull:
        pulled = true;
        Notify(partnerName + " puxou você até ele", 3);
        break;
    default:
        break;
    }
}

bool Session::Partner(double at, PlayerState* out) const
{
    if (!haveLatest || samples.empty())
    {
        return false;
    }

    double wanted = at - settings.interpolationDelay;
    // Before the oldest, or past the newest (packets late): the nearest, not extrapolated far
    if (wanted <= samples.front().at || samples.size() == 1)
    {
        *out = samples.front().state;
        return true;
    }

    for (size_t index = 1; index < samples.size(); index++)
    {
        const Sample& before = samples[index - 1];
        const Sample& after = samples[index];
        if (wanted > after.at)
        {
            continue;
        }

        float t = static_cast<float>((wanted - before.at) / (after.at - before.at));
        *out = t < 0.5f ? before.state : after.state;
        // A jump in area or a teleport: no blending across it
        if (before.state.area == after.state.area)
        {
            for (int axis = 0; axis < 3; axis++)
            {
                out->position[axis] = Lerp(before.state.position[axis], after.state.position[axis], t);
            }

            out->yaw = LerpAngle(before.state.yaw, after.state.yaw, t);
            // The turn: the quaternions blended the short way and made unit again
            const float* a = before.state.rotation;
            const float* b = after.state.rotation;
            float sign = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0 ? -1.0f : 1.0f;
            float length = 0;
            for (int axis = 0; axis < 4; axis++)
            {
                out->rotation[axis] = Lerp(a[axis], b[axis] * sign, t);
                length += out->rotation[axis] * out->rotation[axis];
            }

            if (length > 0)
            {
                float scale = 1.0f / std::sqrt(length);
                for (float& value : out->rotation)
                {
                    value *= scale;
                }
            }
            if (before.state.animation == after.state.animation)
            {
                out->animationTime = Lerp(before.state.animationTime, after.state.animationTime, t);
            }
        }

        return true;
    }

    *out = samples.back().state;
    return true;
}

double Session::PartnerTime(double sentAt, double arrivedAt)
{
    // The quickest packet sets the difference; slower ones nudge it up a little (the clocks drift, the route changes)
    constexpr double Creep = 0.002;
    double sample = arrivedAt - sentAt;
    if (!haveClockOffset || sample < clockOffset)
    {
        clockOffset = sample;
        haveClockOffset = true;
    }
    else
    {
        clockOffset += (sample - clockOffset) * Creep;
    }

    return sentAt + clockOffset;
}

bool Session::PartnerPoses(double at, const std::vector<uint8_t>** from, const std::vector<uint8_t>** to, float* share) const
{
    if (poses.empty() || !Connected())
    {
        return false;
    }

    double wanted = at - settings.interpolationDelay;
    if (wanted <= poses.front().at || poses.size() == 1)
    {
        *from = *to = &poses.front().pose;
        *share = 0;
        return true;
    }

    for (size_t index = 1; index < poses.size(); index++)
    {
        const PoseSample& before = poses[index - 1];
        const PoseSample& after = poses[index];
        if (wanted > after.at)
        {
            continue;
        }

        *from = &before.pose;
        *to = &after.pose;
        double span = after.at - before.at;
        *share = span > 0 ? static_cast<float>((wanted - before.at) / span) : 1.0f;
        return true;
    }

    // Past the newest (late packets): the newest held
    *from = *to = &poses.back().pose;
    *share = 0;
    return true;
}

bool Session::EnterBackpack()
{
    if (!Connected() || inBackpack)
    {
        return false;
    }

    if (PartnerInBackpack())
    {
        Notify("Co-op: " + partnerName + " já está na mochila", 3);
        return false;
    }

    inBackpack = true;
    SendEvent(EventKind::BackpackEnter);
    return true;
}

void Session::LeaveBackpack()
{
    if (!inBackpack)
    {
        return;
    }

    inBackpack = false;
    SendEvent(EventKind::BackpackLeave);
}

bool Session::TakePulled()
{
    bool was = pulled;
    pulled = false;
    return was;
}

bool Session::LeashWantsBackpack(double at, const PlayerState& local)
{
    if (settings.role != Role::Client || settings.leashDistance <= 0 || !Connected() || !haveLatest || inBackpack ||
        latest.Has(PlayerState::Loading) || local.Has(PlayerState::Loading))
    {
        outOfLeashSince = -1;
        return false;
    }

    float dx = local.position[0] - latest.position[0];
    float dy = local.position[1] - latest.position[1];
    float dz = local.position[2] - latest.position[2];
    bool out = local.area != latest.area || dx * dx + dy * dy + dz * dz > settings.leashDistance * settings.leashDistance;
    if (!out)
    {
        outOfLeashSince = -1;
        return false;
    }

    if (outOfLeashSince < 0)
    {
        outOfLeashSince = at;
    }

    return at - outOfLeashSince >= settings.leashSeconds;
}

void Session::ReleaseGate(uint32_t key)
{
    releasedKey = key;
    releasedAt = now;
    holdKey = 0;
    goKey = 0;
}

Session::Gate Session::AskCutscene(uint32_t key, double at)
{
    now = at;
    // Not playing co-op, or alone: as the original
    if (settings.role == Role::Off || !Connected())
    {
        holdKey = 0;
        return Gate::Go;
    }

    // Let go already: Go for the asks of the frames until the script has started it
    if (releasedKey == key && at - releasedAt < 1.0)
    {
        return Gate::Go;
    }

    if (holdKey != key)
    {
        if (holdKey != 0)
        {
            SendEvent(EventKind::CutsceneCancel, holdKey);
        }

        holdKey = key;
        holdSince = at;
        SendEvent(EventKind::CutsceneHold, key);
    }

    holdAsked = at;

    if (settings.role == Role::Client)
    {
        // The host's Go came before this PC reached the cutscene
        if (goKey == key && at - goAt < GoKeepSeconds)
        {
            ReleaseGate(key);
            return Gate::Go;
        }

        return Gate::Hold;
    }

    // The host decides: both there, or the partner riding along in the backpack (it sees what this player sees)
    if (partnerHoldKey == key || PartnerInBackpack())
    {
        SendEvent(EventKind::CutsceneGo, key);
        ReleaseGate(key);
        return Gate::Go;
    }

    return Gate::Hold;
}

Session::GateStatus Session::CutsceneStatus(double at) const
{
    GateStatus status;
    status.holding = holdKey != 0;
    status.heldSeconds = status.holding ? at - holdSince : 0;
    status.partnerHolding = status.holding && partnerHoldKey == holdKey;
    status.canPull = status.holding && status.heldSeconds >= settings.pullOfferSeconds;
    status.partnerWaitsForMe = partnerHoldKey != 0 && partnerHoldKey != holdKey;
    return status;
}

void Session::PullPartner()
{
    SendEvent(EventKind::Pull);
    Notify("Co-op: chamando " + partnerName + "...", 3);
}

void Session::SendWorld(const WorldEvent& event)
{
    if (link != Link::Connected)
    {
        return;
    }

    std::vector<uint8_t> message;
    Writer writer(&message);
    writer.U8(static_cast<uint8_t>(EventKind::World));
    writer.U8(event.kind);
    writer.U32(event.area);
    writer.U32(event.id);
    writer.U32(event.value);
    for (float value : event.position)
    {
        writer.F32(value);
    }

    channel.Queue(message);
}

bool Session::PopWorld(WorldEvent* event)
{
    if (world.empty())
    {
        return false;
    }

    *event = world.front();
    world.pop_front();
    return true;
}

void Session::SendText(const std::string& text) { SendEventText(EventKind::Text, text); }
}
