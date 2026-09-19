#pragma once

#include "Define.h"
#include <cstdint>
#include <string>
#include <vector>

struct AmigoSocialTurn
{
    uint64 speakerGuid = 0;
    std::string speakerName;
    std::string playerText;
    std::string botText;
    uint64 atMs = 0;
};

enum class AmigoSocialIntentKind : uint8_t
{
    None = 0,
    SuggestGoal,
    RequestFollow,
    RequestAssist,
    RequestGather,
    RequestGrind,
    RequestQuest,
    RequestTravel,
    RequestStop
};

struct AmigoSocialIntent
{
    uint64 id = 0;
    AmigoSocialIntentKind kind = AmigoSocialIntentKind::None;
    uint64 sourceGuid = 0;
    std::string sourceName;
    std::string target;
    std::string note;
    uint64 createdAtMs = 0;
    bool acknowledged = false;
};

enum class AmigoPeerDirectiveKind : uint8_t
{
    None = 0,
    Follow,
    Assist
};

struct AmigoPeerDirective
{
    AmigoPeerDirectiveKind kind = AmigoPeerDirectiveKind::None;
    uint64 targetGuid = 0;
    std::string targetName;
};

struct AmigoBotMindSnapshot
{
    std::vector<AmigoSocialTurn> recentTurns;
    AmigoSocialIntent pendingIntent;
    AmigoPeerDirective peerDirective;
    std::string lastSpokenLine;
    std::string lastObservedMission;
    std::string lastObservedActivity;
};

// Thread-safe social/gameplay bridge. It stores copied strings and scalar IDs only;
// it never owns or dereferences AzerothCore objects.
void AmigoMindRecordSocialTurn(uint64 botGuid, AmigoSocialTurn turn, uint32 maxTurns);
uint64 AmigoMindPromoteIntent(uint64 botGuid, AmigoSocialIntent intent);
void AmigoMindAcknowledgeIntent(uint64 botGuid, uint64 intentId);
void AmigoMindSetPeerDirective(uint64 botGuid, AmigoPeerDirective directive);
void AmigoMindClearPeerDirective(uint64 botGuid);
void AmigoMindUpdateGameplay(uint64 botGuid, std::string missionSummary, std::string activitySummary);
AmigoBotMindSnapshot AmigoMindSnapshot(uint64 botGuid);
std::string AmigoMindBuildPromptContext(uint64 botGuid, uint32 maxTurns = 4);
char const* AmigoSocialIntentKindName(AmigoSocialIntentKind kind);
char const* AmigoPeerDirectiveKindName(AmigoPeerDirectiveKind kind);
