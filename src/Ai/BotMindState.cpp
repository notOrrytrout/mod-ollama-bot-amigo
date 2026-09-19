#include "Ai/BotMindState.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace
{
    struct MindState
    {
        std::deque<AmigoSocialTurn> recentTurns;
        AmigoSocialIntent pendingIntent;
        AmigoPeerDirective peerDirective;
        std::string lastSpokenLine;
        std::string lastObservedMission;
        std::string lastObservedActivity;
    };

    std::mutex gMindMutex;
    std::unordered_map<uint64, MindState> gMindByBot;
    std::atomic<uint64> gNextIntentId{1};
}

char const* AmigoSocialIntentKindName(AmigoSocialIntentKind kind)
{
    switch (kind)
    {
        case AmigoSocialIntentKind::SuggestGoal: return "suggest_goal";
        case AmigoSocialIntentKind::RequestFollow: return "request_follow";
        case AmigoSocialIntentKind::RequestAssist: return "request_assist";
        case AmigoSocialIntentKind::RequestGather: return "request_gather";
        case AmigoSocialIntentKind::RequestGrind: return "request_grind";
        case AmigoSocialIntentKind::RequestQuest: return "request_quest";
        case AmigoSocialIntentKind::RequestTravel: return "request_travel";
        case AmigoSocialIntentKind::RequestStop: return "request_stop";
        default: return "none";
    }
}

char const* AmigoPeerDirectiveKindName(AmigoPeerDirectiveKind kind)
{
    switch (kind)
    {
        case AmigoPeerDirectiveKind::Follow: return "follow";
        case AmigoPeerDirectiveKind::Assist: return "assist";
        default: return "none";
    }
}

void AmigoMindRecordSocialTurn(uint64 botGuid, AmigoSocialTurn turn, uint32 maxTurns)
{
    if (!botGuid)
        return;

    std::lock_guard<std::mutex> lock(gMindMutex);
    MindState& state = gMindByBot[botGuid];
    if (!turn.botText.empty())
        state.lastSpokenLine = turn.botText;
    state.recentTurns.push_back(std::move(turn));

    const size_t limit = std::max<uint32>(1, maxTurns);
    while (state.recentTurns.size() > limit)
        state.recentTurns.pop_front();
}

uint64 AmigoMindPromoteIntent(uint64 botGuid, AmigoSocialIntent intent)
{
    if (!botGuid || intent.kind == AmigoSocialIntentKind::None)
        return 0;

    intent.id = gNextIntentId.fetch_add(1);
    intent.acknowledged = false;

    std::lock_guard<std::mutex> lock(gMindMutex);
    gMindByBot[botGuid].pendingIntent = std::move(intent);
    return gMindByBot[botGuid].pendingIntent.id;
}

void AmigoMindAcknowledgeIntent(uint64 botGuid, uint64 intentId)
{
    if (!botGuid || !intentId)
        return;
    std::lock_guard<std::mutex> lock(gMindMutex);
    auto it = gMindByBot.find(botGuid);
    if (it == gMindByBot.end())
        return;
    if (it->second.pendingIntent.id == intentId)
        it->second.pendingIntent.acknowledged = true;
}

void AmigoMindSetPeerDirective(uint64 botGuid, AmigoPeerDirective directive)
{
    if (!botGuid)
        return;
    std::lock_guard<std::mutex> lock(gMindMutex);
    gMindByBot[botGuid].peerDirective = std::move(directive);
}

void AmigoMindClearPeerDirective(uint64 botGuid)
{
    if (!botGuid)
        return;
    std::lock_guard<std::mutex> lock(gMindMutex);
    auto it = gMindByBot.find(botGuid);
    if (it != gMindByBot.end())
        it->second.peerDirective = {};
}

void AmigoMindUpdateGameplay(uint64 botGuid, std::string missionSummary, std::string activitySummary)
{
    if (!botGuid)
        return;
    std::lock_guard<std::mutex> lock(gMindMutex);
    MindState& state = gMindByBot[botGuid];
    state.lastObservedMission = std::move(missionSummary);
    state.lastObservedActivity = std::move(activitySummary);
}

AmigoBotMindSnapshot AmigoMindSnapshot(uint64 botGuid)
{
    AmigoBotMindSnapshot out;
    if (!botGuid)
        return out;

    std::lock_guard<std::mutex> lock(gMindMutex);
    auto it = gMindByBot.find(botGuid);
    if (it == gMindByBot.end())
        return out;

    out.recentTurns.assign(it->second.recentTurns.begin(), it->second.recentTurns.end());
    out.pendingIntent = it->second.pendingIntent;
    out.peerDirective = it->second.peerDirective;
    out.lastSpokenLine = it->second.lastSpokenLine;
    out.lastObservedMission = it->second.lastObservedMission;
    out.lastObservedActivity = it->second.lastObservedActivity;
    return out;
}

std::string AmigoMindBuildPromptContext(uint64 botGuid, uint32 maxTurns)
{
    AmigoBotMindSnapshot state = AmigoMindSnapshot(botGuid);
    std::ostringstream oss;

    if (!state.lastObservedMission.empty())
        oss << "Current mission: " << state.lastObservedMission << "\n";
    if (!state.lastObservedActivity.empty())
        oss << "Current activity: " << state.lastObservedActivity << "\n";

    if (state.peerDirective.kind != AmigoPeerDirectiveKind::None && state.peerDirective.targetGuid)
    {
        oss << "Temporary peer directive: " << AmigoPeerDirectiveKindName(state.peerDirective.kind);
        if (!state.peerDirective.targetName.empty())
            oss << " " << state.peerDirective.targetName;
        oss << ". This is a temporary cooperative request, not ownership or permanent authority.\n";
    }

    if (state.pendingIntent.id && !state.pendingIntent.acknowledged)
    {
        oss << "Pending player intent: " << AmigoSocialIntentKindName(state.pendingIntent.kind);
        if (!state.pendingIntent.sourceName.empty())
            oss << " from " << state.pendingIntent.sourceName;
        if (!state.pendingIntent.target.empty())
            oss << ", target=" << state.pendingIntent.target;
        if (!state.pendingIntent.note.empty())
            oss << ", note=" << state.pendingIntent.note;
        oss << ". This is advisory until validated by Amigo policy.\n";
    }

    const size_t count = std::min<size_t>(maxTurns, state.recentTurns.size());
    if (count)
    {
        oss << "Recent social context:\n";
        const size_t start = state.recentTurns.size() - count;
        for (size_t i = start; i < state.recentTurns.size(); ++i)
        {
            AmigoSocialTurn const& turn = state.recentTurns[i];
            if (!turn.speakerName.empty() || !turn.playerText.empty())
                oss << "- " << (turn.speakerName.empty() ? "player" : turn.speakerName) << ": " << turn.playerText << "\n";
            if (!turn.botText.empty())
                oss << "- bot: " << turn.botText << "\n";
        }
    }

    return oss.str();
}
