#pragma once

#include "Player.h"
#include "ObjectMgr.h"
#include "QuestDef.h"
#include "Script/Playerbots.h"
#include <algorithm>
#include <vector>

// Playerbots uses positive creature entries and negative game-object entries.
inline bool AmigoDropsQuestItem(int32 entry, uint32 itemId)
{
    auto* value = sSharedValueContext.getGlobalValue<std::vector<int32>>(
        "item drop list", itemId);
    if (!value)
        return false;
    auto const sources = value->Get();
    return std::find(sources.begin(), sources.end(), entry) != sources.end();
}

inline bool AmigoNeedsQuestItemFrom(Player* bot, uint32 questId, int32 entry)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest || bot->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
        return false;
    for (uint8 index = 0; index < QUEST_ITEM_OBJECTIVES_COUNT; ++index)
        if (quest->RequiredItemCount[index] &&
            bot->GetItemCount(quest->RequiredItemId[index]) < quest->RequiredItemCount[index] &&
            AmigoDropsQuestItem(entry, quest->RequiredItemId[index]))
            return true;
    return false;
}
