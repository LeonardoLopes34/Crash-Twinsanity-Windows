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

// A script body about to run (behaviours.cpp's ExecuteState, the body that passed): true to keep the script where it is this
// frame, because the body starts a cutscene (CutsceneStart, command 589) and the partner isn't there yet
bool HoldsBody(const StateBody* body, BehaviourRunner* runner, BehaviourLevel* level);

// A model node about to be drawn with its animator's joints (ogi.cpp's ModelNode::Draw): the local player's pose taken to send,
// the partner's puppet given the partner's pose. True when it isn't to be drawn (the local player riding in the backpack)
bool ModelDrawn(ModelNode* node);

// Whether an instance's shadow isn't cast (shadows.cpp's ShadowNode::Update): the local player riding in the backpack
bool HidesShadow(const InstanceContext* instance);
}
