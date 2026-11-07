#pragma once

// Online co-op in the game (COOP.md; coopgame.cpp): the hooks the game's code calls. Off unless $TWIN_COOP (or coop.ini) asks
// for it, and then the game plays as it always did whenever the partner isn't there
struct StateBody;
struct BehaviourRunner;
struct BehaviourLevel;
struct ModelNode;
struct InstanceContext;
struct GraphState;

namespace Coop
{
// Once a frame (the pads' read, platform/native/pads.cpp): the session's network, the local player sent, the backpack and the
// pull applied, the HUD's line
void GameFrame();

// Whether online co-op is on (asked for, whether or not the partner is there)
bool Active();

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
