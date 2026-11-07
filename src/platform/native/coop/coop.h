#pragma once

// Online co-op in the game (COOP.md; coopgame.cpp): the hooks the game's code calls. Off unless $TWIN_COOP (or coop.ini) asks
// for it, and then the game plays as it always did whenever the partner isn't there
struct StateBody;
struct BehaviourRunner;
struct BehaviourLevel;
struct ModelNode;
struct InstanceContext;

namespace Coop
{
// Once a frame (the pads' read, platform/native/pads.cpp): the session's network, the local player sent, the backpack and the
// pull applied, the HUD's line
void GameFrame();

// Whether online co-op is on (asked for, whether or not the partner is there)
bool Active();

// $TWIN_COOP_TEST_RUN=seconds (tests): that long after connecting, the left stick pushed all the way round in a slow circle (the
// character runs in circles, its animations playing as when played), the stick's bytes (0-255, 128 the middle) replaced
void TestStick(unsigned char* x, unsigned char* y);

// A script body about to run (behaviours.cpp's ExecuteState, the body that passed): true to keep the script where it is this
// frame, because the body starts a cutscene (CutsceneStart, command 589) and the partner isn't there yet
bool HoldsBody(const StateBody* body, BehaviourRunner* runner, BehaviourLevel* level);

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

// Whether an instance's shadow isn't cast (shadows.cpp's ShadowNode::Update): the local player riding in the backpack
bool HidesShadow(const InstanceContext* instance);
}
