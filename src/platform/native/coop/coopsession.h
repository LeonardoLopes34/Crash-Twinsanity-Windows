#pragma once

// Online co-op's session (COOP.md): two players, a host and a client, each running the whole game on their own PC with their own
// camera. Each sends its own character as it is; the host has the last word on the world's shared events. The rules of the
// backpack and of the cutscenes' wait live here, apart from the game (coopgame.cpp ties them to it), so they're tested on their own
#include "coopnet.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace Coop
{
enum class Role : uint8_t
{
    Off,
    Host,
    Client,
};

// A player's character as its own PC sees it
struct PlayerState
{
    enum Flags : uint16_t
    {
        Visible = 1 << 0,
        InBackpack = 1 << 1,
        Grounded = 1 << 2,
        Dead = 1 << 3,
        Watching = 1 << 4, // in a cutscene
        Loading = 1 << 5,
    };

    uint32_t tick = 0;
    uint32_t area = 0; // the level (chunk) it's in, as a hash of its name
    float position[3] = {0, 0, 0};
    float yaw = 0;
    float rotation[4] = {0, 0, 0, 1}; // the character's turn as the game keeps it (a quaternion)
    uint16_t model = 0xFFFF;          // the character's model (its OGI's resource), 0xFFFF unknown
    uint16_t animation = 0;
    float animationTime = 0;
    uint16_t flags = 0;
    uint8_t skin = 0; // 0 Crash, 1 Carbon Crash

    bool Has(uint16_t flag) const { return (flags & flag) != 0; }
};

// The events on the reliable channel (the first byte of a message)
enum class EventKind : uint8_t
{
    CutsceneHold = 1,   // "my script is waiting to start cutscene <key>"
    CutsceneCancel = 2, // "it isn't any more" (the player walked off)
    CutsceneGo = 3,     // host: "start it now"
    BackpackEnter = 4,
    BackpackLeave = 5,
    BackpackDenied = 6, // host: "you can't, I'm in it"
    Pull = 7,           // "come here": the receiver goes into the sender's backpack
    World = 8,          // a shared world event (a crate, a pickup, a checkpoint...): kind, id, value
    Text = 9,           // a line to show the other player
};

// A shared world event: what happened (the game's kinds: coopgame.cpp), the area (its chunk's hash), the instance's ID (the game's,
// 0xFFFFFFFF none), a value and where (to find the instance on the other PC)
struct WorldEvent
{
    uint8_t kind = 0;
    uint32_t area = 0;
    uint32_t id = 0;
    uint32_t value = 0;
    float position[3] = {0, 0, 0};
};

// A line for the HUD (coopgame.cpp shows them at the bottom of the screen)
struct Notice
{
    std::string text;
    double until;
};

struct Settings
{
    Role role = Role::Off;
    uint16_t port = DefaultPort;   // the host's port (the client's own is any free one)
    std::string join;              // the client: the host's address ("26.x.x.x" from Radmin VPN)
    std::string name = "Jogador";
    uint8_t skin = 0;
    double stateHz = 60;           // state (and pose) packets a second: the game's own frame rate, so the partner moves as smoothly
    double interpolationDelay = 0.1;
    double timeoutSeconds = 6;
    double pullOfferSeconds = 8;   // waiting at a cutscene this long offers to pull the partner over
    float leashDistance = 0;       // 0: no leash. Past it (or in another area) for leashSeconds, the client goes in the backpack
    double leashSeconds = 4;
};

class Session
{
public:
    enum class Link
    {
        Off,
        Waiting,   // host: no client yet; client: hello sent, no welcome yet
        Connected,
        Failed,
    };

    enum class Gate
    {
        Hold, // keep the script where it is this frame
        Go,   // let it start the cutscene
    };

    // What the cutscene wait shows
    struct GateStatus
    {
        bool holding = false;      // this PC is waiting at a cutscene
        double heldSeconds = 0;
        bool partnerHolding = false; // the partner waits at the same one
        bool canPull = false;      // waited long enough to offer the pull
        bool partnerWaitsForMe = false; // the partner waits at a cutscene this PC hasn't reached
    };

    bool Start(const Settings& settings);
    void Stop();
    // Once a frame: the network read and written, timeouts, the state sent. The local player's character as it is now
    void Update(double now, const PlayerState& local);

    Role GetRole() const { return settings.role; }
    bool IsHost() const { return settings.role == Role::Host; }
    Link GetLink() const { return link; }
    bool Connected() const { return link == Link::Connected; }
    const std::string& Error() const { return error; }
    const std::string& PartnerName() const { return partnerName; }
    uint8_t PartnerSkin() const { return partnerSkin; }
    double RoundTrip() const { return roundTrip; }

    // The partner's character as it was InterpolationDelay ago, blended between the two states around then. False with none yet
    bool Partner(double now, PlayerState* out) const;
    // The newest state, as it came
    const PlayerState& PartnerLatest() const { return latest; }

    // The character's pose (its joints, packed by the game: coopgame.cpp), sent with the state; and the partner's newest
    const std::vector<uint8_t>& PartnerPose() const { return partnerPose; }
    void SetLocalPose(const std::vector<uint8_t>& pose) { localPose = pose; }
    // The two poses around the moment the partner is drawn at (InterpolationDelay ago, on the partner's clock) and the share of
    // the way from the first to the second: false with none yet. The same moment as Partner's, so the pose and the place agree
    bool PartnerPoses(double now, const std::vector<uint8_t>** from, const std::vector<uint8_t>** to, float* share) const;

    // The backpack: going in (false when not allowed now), coming out
    bool EnterBackpack();
    void LeaveBackpack();
    bool InBackpack() const { return inBackpack; }
    bool PartnerInBackpack() const { return latest.Has(PlayerState::InBackpack) && Connected(); }
    // Asked by the game when the partner pulls this player over (the game puts it in the backpack and clears it)
    bool TakePulled();
    // The leash (Settings::leashDistance): whether this PC's player should go in the backpack now (the client only)
    bool LeashWantsBackpack(double now, const PlayerState& local);

    // The cutscene wait: asked every frame while a script waits to start cutscene <key>. Go when both players are there (or the
    // partner is in the backpack, or gone)
    Gate AskCutscene(uint32_t key, double now);
    GateStatus CutsceneStatus(double now) const;
    // Pulls the partner over (offered once waited pullOfferSeconds, or when the partner waits for this player)
    void PullPartner();

    // Shared world events: sent (the host's are the truth), and those that came
    void SendWorld(const WorldEvent& event);
    bool PopWorld(WorldEvent* event);
    void SendText(const std::string& text);

    // The HUD's lines (Portuguese)
    const std::deque<Notice>& Notices() const { return notices; }
    void Notify(const std::string& text, double seconds = 4);

private:
    void Receive(double now);
    void Handle(const Address& from, const uint8_t* data, size_t size, double now);
    void HandleEvent(const uint8_t* data, size_t size, double now);
    void SendEvent(EventKind kind, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0);
    void SendEventText(EventKind kind, const std::string& text);
    void SendRaw(const std::vector<uint8_t>& packet);
    void SendState(const PlayerState& local);
    void Connect(const Address& peer, double now);
    void Disconnect(const std::string& why);
    void ReleaseGate(uint32_t key);
    // A time on the partner's clock placed on this PC's: by the clocks' difference, taken from the quickest packets (the least
    // delayed), creeping up slowly when every packet comes later
    double PartnerTime(double sentAt, double arrivedAt);

    Settings settings;
    UdpSocket socket;
    Link link = Link::Off;
    std::string error;
    Address peer;
    Address hostAddress;
    ReliableChannel channel;
    double lastHeard = 0;
    double lastHello = 0;
    double lastState = 0;
    double lastPing = 0;
    double roundTrip = 0;
    double now = 0;
    uint32_t tick = 0;
    std::string partnerName;
    uint8_t partnerSkin = 0;

    struct Sample
    {
        double at;
        PlayerState state;
    };
    std::deque<Sample> samples;
    PlayerState latest;
    bool haveLatest = false;

    bool inBackpack = false;
    bool pulled = false;
    double outOfLeashSince = -1;

    // The cutscene wait
    uint32_t holdKey = 0;
    double holdSince = 0;
    double holdAsked = 0;
    uint32_t partnerHoldKey = 0;
    uint32_t goKey = 0;     // a Go for a cutscene this PC hasn't reached yet (the client)
    double goAt = 0;
    uint32_t releasedKey = 0; // the cutscene let go: Go for its asks until it starts
    double releasedAt = 0;

    std::deque<WorldEvent> world;
    std::deque<Notice> notices;
    std::vector<uint8_t> localPose;
    std::vector<uint8_t> partnerPose;
    uint32_t partnerPoseTick = 0;
    struct PoseSample
    {
        double at;
        std::vector<uint8_t> pose;
    };
    std::deque<PoseSample> poses;
    double clockOffset = 0;
    bool haveClockOffset = false;
};

// A cutscene's key from what names it (the director's object, the graph): FNV-1a
uint32_t HashKey(const char* text, uint32_t seed = 2166136261u);
}
