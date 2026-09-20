#pragma once

#include "ScriptMgr.h"
#include "Player.h"
#include "Define.h"

bool HasAmigoPendingControl(uint64 botGuid);

class AmigoControlControllerScript : public PlayerScript
{
public:
    AmigoControlControllerScript();
    // Pull the next ControlActionState and enqueue a Playerbot command if valid.
    void OnPlayerAfterUpdate(Player* player, uint32 diff) override;
};
