#pragma once

#include "ScriptMgr.h"
#include <string>

class Group;
class Player;

// Party/whisper social surface for the configured Amigo bot. Gameplay actions
// are never executed here; player requests are recorded as structured social
// intent for the planner/policy layer.
class AmigoSocialScript : public PlayerScript
{
public:
    AmigoSocialScript();

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Player* receiver) override;
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Group* group) override;
};

class AmigoEventChatterScript : public PlayerScript
{
public:
    AmigoEventChatterScript();
    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override;
    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override;
    void OnPlayerJustDied(Player* player) override;
};
