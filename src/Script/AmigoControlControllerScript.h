#pragma once

#include "ScriptMgr.h"
#include "Player.h"
#include "Define.h"

bool HasAmigoPendingControl(uint64 botGuid);
void ClearAmigoPendingControl(uint64 botGuid);

class AmigoControlControllerScript : public PlayerScript
{
public:
    AmigoControlControllerScript();
    // Pull the next ControlActionState and enqueue a Playerbot command if valid.
    void OnPlayerAfterUpdate(Player* player, uint32 diff) override;
    void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid lootGuid) override;
};
