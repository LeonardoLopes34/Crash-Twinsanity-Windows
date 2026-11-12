#pragma once

// Online co-op in the game (COOP.md; coopgame.cpp): the hooks the game's code calls. Off unless $TWIN_COOP (or coop.ini) asks
// for it, and then the game plays as it always did whenever the partner isn't there
struct StateBody;
struct BehaviourRunner;
struct BehaviourLevel;
struct ModelNode;
struct InstanceContext;
struct Reference;
struct ChunkData;
struct GraphState;
struct Matrix4x4;
struct Vector4;

namespace Coop
{
// Once a frame (the pads' read, platform/native/pads.cpp): the session's network, the local player sent, the backpack and the
// pull applied, the HUD's line
void GameFrame();

// Whether online co-op is on (asked for, whether or not the partner is there)
bool Active();
// The local player's own character on this PC (even while g_PlayerInstance is swapped to the second player/partner)
InstanceContext* LocalPlayer();
// The online partner's character on this PC (none while not in play)
InstanceContext* NetworkPartner();
// Whether g_PlayerInstance is currently swapped to the second player/partner during BeginObjectUpdate
bool SwappedToSecond();
// Online session queries used by shared co-op features
bool IsHost();
int LocalSkin();
int PartnerSkin();
bool InPartnerBackpack();
bool PartnerInMyBackpack();
// Whether an object should keep stepping even when outside the local camera's sight (near the partner or attacked by them)
bool ShouldStepObject(const InstanceContext* instance);
bool IsReplayedObject(const InstanceContext* instance);
// 3D ping spot shared between the two PCs
void SendPingSpot(float x, float y, float z);
void ReceivePingSpot(float x, float y, float z);
float OnlineLeashWarning();

// The second player on this PC (coop/secondplayer.cpp; coop.ini's players=2 or $TWIN_PLAYERS=2): a second character of its own,
// played with the second gamepad (the pads' port 1). Once a frame from the pads' read; and while it's made (instancefactory.cpp's
// GameFactoryStandIn: a character of its own, not the progress's)
void SecondPlayerFrame();
extern bool g_SpawningSecondPlayer;
// Whether the second player is asked for (the pads give a lone gamepad to the second player then)
bool SecondPlayerWanted();
// The second player's character (none while it isn't in play)
InstanceContext* SecondPlayer();
// A reference to the second player on foot in the world, for the chunk loading to follow the links from its chunk too
// (chunkloading.cpp: the world around both players loaded and stepped). None online, while it rides in the backpack or isn't
// in play, and with $TWIN_ONE_FOCUS (the world around the first player only, as before)
Reference* SecondPlayerReference();
// The character an instance's sight and aim go to (conditionchecks.cpp: enemies looking for the player): the nearer of the two
// players on foot (the played one when there's no second, or the second rides in the backpack)
InstanceContext* NearerPlayer(InstanceContext* from, InstanceContext* played);
// The player a script's "the player" means for the instance asking (objectnode.cpp, objectnodehelpers.cpp, motion.cpp): for a
// character that isn't one of the two players (Cortex, Nina following or tied to the player) the nearer player, else the player
InstanceContext* PlayerFor(InstanceContext* asking, InstanceContext* player);
// Around each node's update as the world steps (instances.cpp): "the player" (g_PlayerInstance, the progress's character played)
// the second player for the node of an instance nearer to it (or its own), given back after
void BeginObjectUpdate(InstanceContext* owner);
// A sound's distance share for the second player's ears (soundobjects.cpp: the one mix both hear, through Parsec): the sound at a
// point, the listener the first player's camera; under 1 when the second player's camera is nearer (the sound's place in the
// listener's space scaled by it: as loud as it is to the nearer of the two)
float SoundNearness(const Vector4* at, InstanceContext* listener);
// The camera that hears a sound in a chunk other than the listener's (soundobjects.cpp): the second player's, when the sound is in
// the area its camera is in (its jumps and spins, what's around it far from the first player: they were silent, the first
// camera's view never drawing that area). None otherwise: the listener hears it as before
InstanceContext* SecondEarFor(ChunkData* chunk, InstanceContext* listener);
void EndObjectUpdate();
// Once a frame as it's handed to be shown (graphics/display.cpp's ShowFrame): which player's view the frame just drawn is (0 the
// first's, 1 the second's: the second window), the next frame's camera chosen
int TwoViewsFrame();
// Two players on this PC: when the next frame should start (SDL_GetTicksNS's clock), so each window gets its own even rate (the
// first's 90 a second, the second's 60: $TWIN_P1_FPS, $TWIN_P2_FPS); 0 when that's not this module's to say (graphics/display.cpp's
// WaitForVBlank paces as usual then). Taken once: it reads 0 until the next TwoViewsFrame
unsigned long long TakeTwoViewsPace();
// The frame rate the two windows' rates make together (the game clock's frames a second), 0 without a second player
int TwoViewsFrameRate();
// (TwoViewsFrame's answer while there's one view and a second player's window: the picture shown in both windows)
constexpr int BothViews = 2;
// Around the game's drawing (context.cpp's Render): the second player's camera given to the view only for the drawing of its frames,
// the first player's given back right after (the game's own steps, triggers, rigs and chunk loading only ever see the first's)
void BeforeDraw();
void AfterDraw();
// Whether two views are drawn (each frame one player's camera), and whether the drawing now is the second player's. With two views
// what one camera sees isn't taken back by the other's drawing (scenery.cpp's culling, ogi.cpp's models, shadows.cpp): an instance
// in a drawn cell for either camera stays drawn
bool TwoViews();
bool DrawingSecondView();
// Whether the last frame drawn was the second player's view (its chunks' draw matrices and drawn stamps are that camera's)
bool LastDrawnSecondView();
// An instance met by a camera's drawing (scenery.cpp's culling: in view or not, at a distance): its drawn cell flag and its seen
// stamp (the distance the game's update rates go by) made both cameras' (in view of either, the nearer distance)
void NoteSeen(InstanceContext* instance, unsigned distance, bool inView);
// Whether the second camera saw an instance at its last drawings (ogi.cpp's models, shadows.cpp, agentnodes.cpp)
bool SeenBySecondView(const InstanceContext* instance);
// The backpack between the two players on this PC (Select, F5): whether a model or a shadow is the rider's (not drawn)
// How the second player's gamepad shares the game's own pad (platform/native/pads.cpp): in play only its Start; in a pause the
// pauser's alone; elsewhere (the menus, cutscenes, loads) either player's buttons
enum SharedPad : int
{
    PadPlaying = 0,
    PadPaused = 1,
    PadMenus = 2,
};
int SharedPadMode();
// How near the two players are to the leash's limit (0 well within, 1 at it): the windows' edges drawn red (graphics/display.cpp)
float LeashWarning();
// The ping both players see (a player's camera's look marked: the first's middle mouse button, the second's R3): where it is in a
// view's picture (view 0 the first player's window, 1 the second's; -1 to 1, the picture's middle 0), how strong, and whether it's
// off to the picture's edge (out of view); false when there's none
bool PingMarker(int view, float* x, float* y, float* strength, bool* edge, float* scale, float* squash);
// Whether a player (0 the first, 1 the second) asked for a small picture of the other's view in a corner of their window
bool PipWanted(int viewer);
// The minimap's circle for a view (0 the first player's window, 1 the second's): dots as four floats each (x and y from -1 to 1 on
// the circle, the kind: 1 enemy, 2 crate, 3 wumpa, 4 other character, 5 the other player, 6 the ping; and one unused), how many,
// and which way the view's own character faces on it (radians, 0 up). False when there's none to draw
bool MinimapDots(int view, const float** dots, int* count, float* facing);
// A body that starts a cutscene (coopgame.cpp's HoldsBody), the instance that runs it: true to hold it back (secondplayer.cpp)
bool CutsceneWaitHolds(InstanceContext* director);
// The game's bottom line of text (a short sentence, no accents; "" hides)
void ShowBottomLine(const char* text);
bool RidingHides(const ModelNode* node);
// The second player's character drawn green (ogi.cpp's ModelNode::Draw): whether a model is its, and a light's colour made green
bool TintsGreen(const ModelNode* node);
void Greened(float* red, float* green, float* blue);
bool RidingHidesShadow(const InstanceContext* instance);
// The camera matrix a character's stick is read against (controls.cpp's CharacterControls::Frame): with the second player in play,
// the camera that character's player sees (the game reads the chunk's last drawn camera, the other player's half the frames); none
// otherwise (the game's own)
const Matrix4x4* MoveView(InstanceContext* character);

// The free camera (followrig.cpp; F7 turns it on and off, coop.ini's camera=original starts it off): on foot, the camera turns only
// when the player turns it (the right stick, the mouse; the shoulder buttons still swing it behind the character), never on its
// own nor to the levels' camera triggers. With a vehicle (the Humiliskate, the Rollerbrawl, the hoverboard) the game's own
bool FreeCamera();
// Once a frame from wherever the keys are read (F7), even without co-op
void CameraKeys();

// The pause shared (gamecontroller.cpp): the partner paused (this game pauses as for its start button; once), the partner
// resumed their pause (this game resumes; once), and whether this player may not resume (the partner's pause: only they do)
bool TakePartnerPause();
bool TakePartnerResume();
bool BlocksResume();

// $TWIN_COOP_TEST_RUN=seconds (tests): that long after connecting, the left stick pushed all the way round in a slow circle (the
// character runs in circles, its animations playing as when played), the stick's bytes (0-255, 128 the middle) replaced
void TestStick(unsigned char* x, unsigned char* y);

// A script body about to run (behaviours.cpp's ExecuteState, the body that passed): true to keep the script where it is this
// frame, because the body starts a cutscene (CutsceneStart, command 589) and the partner isn't there yet
bool HoldsBody(const StateBody* body, BehaviourRunner* runner, BehaviourLevel* level);

// A body behaviours.cpp's ExecuteState runs once (a state's completion body, its own body) held back for a cutscene's wait: kept
// for its level (and the state it's in), asked about again every frame, run once the partner is there
enum HeldWay : int
{
    HeldCompletion = 1,
    HeldOwn = 2,
};
StateBody* HeldBody(const BehaviourLevel* level, const GraphState* state, int* way);
void HoldBody(const BehaviourLevel* level, const GraphState* state, StateBody* body, int way);
void ReleaseHeldBody(const BehaviourLevel* level);

// A model node about to be drawn with its animator's joints (ogi.cpp's ModelNode::Draw): the local player's pose taken to send,
// the partner's puppet given the partner's pose. True when it isn't to be drawn (the local player riding in the backpack)
bool ModelDrawn(ModelNode* node);

// The world shared (crates and wumpa fruit):
// - an agent attacked (agents.cpp's BasicAgent::Attacked): a crate the local player attacked told the partner's game, which
//   attacks its own the same way;
// - a crate's contents about to be made (commandsnode.cpp's CreateCrateContents): true to leave them out (a crate the partner
//   broke: the partner's game has them);
// - wumpa fruit about to be added (commandsgame.cpp's PickUpWumpa): told the partner's game (which adds them too and hides the
//   pickup), true to leave them out (from a crate the partner broke)
void AgentAttacked(InstanceContext* instance, unsigned kind, InstanceContext* sender);
bool SuppressesCrateContents(const InstanceContext* crate);
bool WumpaPicked(InstanceContext* from, unsigned count);
// A gem (its colour) or the level's crystal found (commandsgame.cpp's AddGem, AddCrystal; from: the instance whose script found it),
// told the partner's game; and the partner's found here (the same progress mark and HUD, not told back: commandsgame.cpp)
void CollectibleFound(bool gem, int colour, InstanceContext* from);
void AwardGem(int colour);
void AwardCrystal();
extern bool g_Awarding;

// Whether an instance's shadow isn't cast (shadows.cpp's ShadowNode::Update): the local player riding in the backpack
bool HidesShadow(const InstanceContext* instance);
}
