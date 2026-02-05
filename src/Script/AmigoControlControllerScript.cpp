#include "Script/AmigoControlControllerScript.h"
#include "Ai/ControlAction.h"
#include "Bot/BotControlApi.h"
#include "Script/OllamaBotConfig.h"
#include "Ai/OllamaRuntime.h"
#include "Bot/BotMovement.h"
#include "Bot/BotNavState.h"
#include "Util/WorldChecks.h"
#include "ObjectMgr.h"
#include "GameObject.h"
#include "ObjectAccessor.h"
#include "Bot/BotTravel.h"
#include "Bot/BotProfession.h"
#include "Log.h"
#include "Util/PlayerbotsCompat.h"
#include "SharedDefines.h"
#include "QuestDef.h"
#include "Timer.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <unordered_map>

#include "Script/OllamaBotPlannerRefresh.h"

namespace
{
    // Upper bound for move hop distances to avoid extreme leaps.
    constexpr float kMaxMoveDistanceCap = 325.0f;
    constexpr uint32 kQuestTurnInFollowupTimeoutMs = 30000;     // 30s
    constexpr uint32 kQuestTurnInPlannerGuardMs = 10000;        // 10s

    struct BotSnapshot
    {
        // Lightweight snapshot used for gating control actions.
        Position3 pos;
        bool inCombat = false;
        bool grindMode = false;
        bool isMoving = false;
    };

    bool CanMoveNow(BotSnapshot const& snapshot)
    {
        // Avoid starting travel movement while combat/grind/movement is active.
        return !snapshot.inCombat && !snapshot.grindMode && !snapshot.isMoving;
    }

    BotSnapshot BuildBotSnapshot(Player* bot)
    {
        // Capture only the properties needed for control gating.
        BotSnapshot snapshot;
        snapshot.pos = Position3{ bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ() };
        snapshot.inCombat = bot->IsInCombat();
        snapshot.isMoving = bot->isMoving();

        std::string currentActivity;
        std::string activityReason;
        if (TryGetActivityState(bot, currentActivity, activityReason))
        {
            std::string lower = currentActivity;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            snapshot.grindMode = (lower == "grind");
        }

        return snapshot;
    }

    const char* CapabilityName(ControlAction::Capability capability)
    {
        // Human-readable labels for logging.
        switch (capability)
        {
            case ControlAction::Capability::Idle:               return "idle";
            case ControlAction::Capability::MoveHop:            return "move_hop";
            case ControlAction::Capability::EnterGrind:         return "enter_grind";
            case ControlAction::Capability::StopGrind:          return "stop_grind";
            case ControlAction::Capability::Stay:               return "stay";
            case ControlAction::Capability::Unstay:             return "unstay";
            case ControlAction::Capability::TalkToQuestGiver:   return "talk_to_quest_giver";
            case ControlAction::Capability::Fish:               return "fish";
            case ControlAction::Capability::UseProfession:      return "profession";
            case ControlAction::Capability::TurnLeft90:         return "turn_left_90";
            case ControlAction::Capability::TurnRight90:        return "turn_right_90";
            case ControlAction::Capability::TurnAround:         return "turn_around";
            default:                                             return "unknown";
        }
    }

    std::string NormalizeToken(std::string value)
    {
        size_t start = value.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
        {
            return {};
        }
        size_t end = value.find_last_not_of(" \t\r\n");
        value = value.substr(start, end - start + 1);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    bool TryMapSkill(std::string const& skill, uint32& outSkillId)
    {
        std::string s = NormalizeToken(skill);
        if (s == "alchemy") { outSkillId = SKILL_ALCHEMY; return true; }
        if (s == "blacksmithing") { outSkillId = SKILL_BLACKSMITHING; return true; }
        if (s == "enchanting") { outSkillId = SKILL_ENCHANTING; return true; }
        if (s == "engineering") { outSkillId = SKILL_ENGINEERING; return true; }
        if (s == "herbalism") { outSkillId = SKILL_HERBALISM; return true; }
        if (s == "inscription") { outSkillId = SKILL_INSCRIPTION; return true; }
        if (s == "jewelcrafting") { outSkillId = SKILL_JEWELCRAFTING; return true; }
        if (s == "leatherworking") { outSkillId = SKILL_LEATHERWORKING; return true; }
        if (s == "mining") { outSkillId = SKILL_MINING; return true; }
        if (s == "skinning") { outSkillId = SKILL_SKINNING; return true; }
        if (s == "tailoring") { outSkillId = SKILL_TAILORING; return true; }
        if (s == "cooking") { outSkillId = SKILL_COOKING; return true; }
        if (s == "first aid" || s == "first_aid") { outSkillId = SKILL_FIRST_AID; return true; }
        if (s == "fishing") { outSkillId = SKILL_FISHING; return true; }
        return false;
    }

    bool QuestGiverMatchesQuestId(Player* bot, WorldObject* questGiver, uint32 questId)
    {
        if (!bot || !questGiver || questId == 0)
        {
            return false;
        }
        if (!bot->CanInteractWithQuestGiver(questGiver))
        {
            return false;
        }

        auto boundsContainQuest = [&](QuestRelationBounds bounds) -> bool
        {
            for (auto it = bounds.first; it != bounds.second; ++it)
            {
                if (it->second == questId)
                {
                    return true;
                }
            }
            return false;
        };

        if (Creature* creature = questGiver->ToCreature())
        {
            uint32 entry = creature->GetEntry();
            if (boundsContainQuest(sObjectMgr->GetCreatureQuestRelationBounds(entry)))
            {
                return true;
            }
            return boundsContainQuest(sObjectMgr->GetCreatureQuestInvolvedRelationBounds(entry));
        }
        if (GameObject* go = questGiver->ToGameObject())
        {
            uint32 entry = go->GetEntry();
            if (boundsContainQuest(sObjectMgr->GetGOQuestRelationBounds(entry)))
            {
                return true;
            }
            return boundsContainQuest(sObjectMgr->GetGOQuestInvolvedRelationBounds(entry));
        }
        return false;
    }

    WorldObject* FindBestQuestGiverForQuestId(Player* bot, PlayerbotAI* ai, uint32 questId)
    {
        if (!bot || !ai || questId == 0)
        {
            return nullptr;
        }
        AiObjectContext* context = ai->GetAiObjectContext();
        if (!context)
        {
            return nullptr;
        }

        WorldObject* best = nullptr;
        float bestDist = std::numeric_limits<float>::max();

        GuidVector npcs = context->GetValue<GuidVector>("nearest npcs")->Get();
        for (ObjectGuid const& guid : npcs)
        {
            Creature* creature = ai->GetCreature(guid);
            if (!creature || !creature->IsQuestGiver())
            {
                continue;
            }
            if (!QuestGiverMatchesQuestId(bot, creature, questId))
            {
                continue;
            }
            float dist = bot->GetDistance(creature);
            if (dist < bestDist)
            {
                best = creature;
                bestDist = dist;
            }
        }

        GuidVector gos = context->GetValue<GuidVector>("nearest game objects")->Get();
        for (ObjectGuid const& guid : gos)
        {
            GameObject* go = ai->GetGameObject(guid);
            if (!go || go->GetGoType() != GAMEOBJECT_TYPE_QUESTGIVER)
            {
                continue;
            }
            if (!QuestGiverMatchesQuestId(bot, go, questId))
            {
                continue;
            }
            float dist = bot->GetDistance(go);
            if (dist < bestDist)
            {
                best = go;
                bestDist = dist;
            }
        }

        return best;
    }

    struct PendingQuestGiverFollowup
    {
        uint32 questId = 0;
        ObjectGuid questGiverGuid;
        uint32 startedMs = 0;
        QuestStatus initialStatus = QUEST_STATUS_NONE;
        bool initialRewarded = false;
        uint32 lastAcceptAttemptMs = 0;
        uint8 acceptAttempts = 0;
        bool acceptDone = false;
        bool ltgRefreshDone = false;
    };

    std::unordered_map<uint64, PendingQuestGiverFollowup> pendingQuestGiverFollowups;
    std::unordered_map<uint64, uint32> lastPlannerRefreshMs;

    void MaybeHandleQuestGiverFollowup(Player* bot, PlayerbotAI* ai)
    {
        if (!bot || !ai)
        {
            return;
        }
        uint64 guid = bot->GetGUID().GetRawValue();
        auto it = pendingQuestGiverFollowups.find(guid);
        if (it == pendingQuestGiverFollowups.end())
        {
            return;
        }

        PendingQuestGiverFollowup& pending = it->second;
        uint32 nowMs = getMSTime();
        if (pending.startedMs == 0 || nowMs - pending.startedMs > kQuestTurnInFollowupTimeoutMs)
        {
            pendingQuestGiverFollowups.erase(it);
            return;
        }
        if (pending.questId == 0 || pending.questGiverGuid.IsEmpty())
        {
            pendingQuestGiverFollowups.erase(it);
            return;
        }

        auto reselectionOk = [&]()
        {
            if (Creature* creature = ObjectAccessor::GetCreature(*bot, pending.questGiverGuid))
            {
                bot->SetSelection(creature->GetGUID());
                return true;
            }
            if (GameObject* gameObject = ObjectAccessor::GetGameObject(*bot, pending.questGiverGuid))
            {
                bot->SetTarget(gameObject->GetGUID());
                return true;
            }
            return false;
        };

        auto tryAcceptAll = [&]()
        {
            if (!reselectionOk())
            {
                return;
            }

            // Auto-accept all available quests from this quest giver.
            BotControlCommand acceptCmd;
            acceptCmd.type = BotControlCommandType::PlayerbotCommand;
            // Playerbots' "accept" chat command maps to AcceptQuestAction and only accepts anything when:
            // - a quest id is provided, or
            // - the special param "*" is provided (accept all quests from nearby quest givers in interaction range).
            // Without a param it will no-op, which looks like "it listed quests but didn't accept them".
            acceptCmd.args = { "accept *" };
            EnqueueBotControlCommand(bot, acceptCmd, "auto_accept_all_on_talk");
            pending.lastAcceptAttemptMs = nowMs;
            if (pending.acceptAttempts < std::numeric_limits<uint8>::max())
            {
                pending.acceptAttempts++;
            }
        };

        // Turn-in path: wait for quest reward, then accept all and refresh LTG (guarded).
        if (pending.initialStatus == QUEST_STATUS_COMPLETE && !pending.initialRewarded)
        {
            bool rewardedNow = bot->GetQuestRewardStatus(pending.questId);
            if (!rewardedNow)
            {
                return;
            }
            if (!pending.acceptDone)
            {
                tryAcceptAll();
                pending.acceptDone = true;
            }

            // Request an LTG refresh, guarded to avoid spamming when turning in many quests quickly.
            if (!pending.ltgRefreshDone)
            {
                uint32 lastRefresh = 0;
                auto ltgIt = lastPlannerRefreshMs.find(guid);
                if (ltgIt != lastPlannerRefreshMs.end())
                {
                    lastRefresh = ltgIt->second;
                }
                if (lastRefresh == 0 || nowMs - lastRefresh >= kQuestTurnInPlannerGuardMs)
                {
                    lastPlannerRefreshMs[guid] = nowMs;
                    RequestLongTermPlannerRefresh(guid, nowMs);
                    pending.ltgRefreshDone = true;
                }
            }

            pendingQuestGiverFollowups.erase(it);
            return;
        }

        // Accept path: after talking to a quest giver for an available quest, try "accept" a few times.
        if (pending.initialStatus == QUEST_STATUS_NONE)
        {
            QuestStatus current = bot->GetQuestStatus(pending.questId);
            if (current != QUEST_STATUS_NONE)
            {
                pendingQuestGiverFollowups.erase(it);
                return;
            }

            // Give the "talk" command a moment to open gossip/quest dialog before accepting.
            constexpr uint32 kAcceptInitialDelayMs = 750;
            constexpr uint32 kAcceptRetryEveryMs = 2000;
            constexpr uint8 kMaxAcceptAttempts = 3;

            if (pending.acceptAttempts >= kMaxAcceptAttempts)
            {
                pendingQuestGiverFollowups.erase(it);
                return;
            }

            if (pending.acceptAttempts == 0)
            {
                if (nowMs - pending.startedMs >= kAcceptInitialDelayMs)
                {
                    tryAcceptAll();
                }
                return;
            }

            if (nowMs - pending.lastAcceptAttemptMs >= kAcceptRetryEveryMs)
            {
                tryAcceptAll();
            }
        }
    }
}

AmigoControlControllerScript::AmigoControlControllerScript()
    : PlayerScript("AmigoControlControllerScript")
{
}

void AmigoControlControllerScript::OnPlayerAfterUpdate(Player* player, uint32 /*diff*/)
{
    // Drain queued control actions and translate them to Playerbot commands.
    if (!player || !g_OllamaBotRuntime.enable_control || !player->IsInWorld())
    {
        return;
    }

    PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(player);
    if (!ai || !ai->IsBotAI())
    {
        return;
    }

    if (!g_OllamaBotControlBotName.empty() && player->GetName() != g_OllamaBotControlBotName)
    {
        return;
    }

    // Follow up on prior quest giver interactions even if no new control action is dequeued this tick.
    MaybeHandleQuestGiverFollowup(player, ai);

    ControlActionState actionState;
    uint64 guid = player->GetGUID().GetRawValue();
    if (!ControlActionRegistry::Instance().TryDequeue(guid, actionState))
    {
        return;
    }

    BotSnapshot snapshot = BuildBotSnapshot(player);

    if (snapshot.isMoving &&
        actionState.action.capability != ControlAction::Capability::Idle &&
        actionState.action.capability != ControlAction::Capability::StopGrind)
    {
        // When the bot is being manually moved (e.g. playerbots "bot self"), do not inject actions.
        LOG_INFO("server.loading", "[OllamaBotAmigo] Ignored control action while bot is moving for {}", player->GetName());
        return;
    }

    if (snapshot.inCombat)
    {
        LOG_INFO("server.loading", "[OllamaBotAmigo] Ignored control action during combat for {}", player->GetName());
        return;
    }

    if (actionState.action.capability == ControlAction::Capability::MoveHop)
    {
        // Move hops start a stateful, path-based movement.
        if (!CanMoveNow(snapshot))
        {
            LOG_INFO("server.loading", "[OllamaBotAmigo] Rejecting move_hop due to grind/moving/combat for {}", player->GetName());
            return;
        }

        // Resolve the LLM-selected navigation candidate to an engine destination.
        // The LLM never provides or sees coordinates.
        WorldPosition dest(player);
        bool candReachable = false;
        bool candHasLOS = false;
        bool candCanMove = false;

        if (actionState.action.navCandidateId.empty())
        {
            LOG_INFO("server.loading", "[OllamaBotAmigo] Rejecting move_hop: missing candidate_id for {}", player->GetName());
            return;
        }

        if (!BotNavStateRegistry::TryResolve(guid,
                                             actionState.action.navEpoch,
                                             actionState.action.navCandidateId,
                                             dest,
                                             candReachable,
                                             candHasLOS,
                                             candCanMove))
        {
            LOG_INFO(
                "server.loading",
                "[OllamaBotAmigo] Rejecting move_hop: failed to resolve candidate (epoch/candidate mismatch) for {} (nav_epoch={}, candidate_id={})",
                player->GetName(),
                actionState.action.navEpoch,
                actionState.action.navCandidateId
            );
            return;
        }

        if (!candCanMove)
        {
            LOG_INFO(
                "server.loading",
                "[OllamaBotAmigo] Rejecting move_hop: candidate cannot_move for {} (nav_epoch={}, candidate_id={})",
                player->GetName(),
                actionState.action.navEpoch,
                actionState.action.navCandidateId
            );
            return;
        }
        if (!candReachable)
        {
            LOG_INFO(
                "server.loading",
                "[OllamaBotAmigo] Rejecting move_hop: candidate unreachable for {} (nav_epoch={}, candidate_id={})",
                player->GetName(),
                actionState.action.navEpoch,
                actionState.action.navCandidateId
            );
            return;
        }

        LOG_INFO(
            "server.loading",
            "[OllamaBotAmigo] move_hop accepted for {}: nav_epoch={} candidate_id={} reachable={} los={} reasoning='{}'",
            player->GetName(),
            actionState.action.navEpoch,
            actionState.action.navCandidateId,
            candReachable ? "yes" : "no",
            candHasLOS ? "yes" : "no",
            actionState.reasoning
        );

        BotMovement* movement = BotMovementRegistry::Get(guid);
        BotTravel* travel = BotTravelRegistry::Get(guid);
        if (!movement)
        {
            LOG_INFO("server.loading", "[OllamaBotAmigo] No movement instance registered for {}", player->GetName());
            return;
        }
        if (!travel)
        {
            LOG_INFO("server.loading", "[OllamaBotAmigo] No travel instance registered for {}", player->GetName());
            return;
        }

        if (travel->Active())
        {
            LOG_INFO("server.loading", "[OllamaBotAmigo] Rejecting move_hop: travel already active for {}", player->GetName());
            return;
        }

        // Pre-validate physical feasibility using Playerbots-style engine helpers.
        // This reduces impossible tool calls (e.g., points inside terrain or behind unreached geometry).
        bool reachable = WorldChecks::CanReach(player, dest);
        bool hasLOS = WorldChecks::IsWithinLOS(player, dest);
        if (!reachable)
        {
            LOG_INFO(
                "server.loading",
                "[OllamaBotAmigo] Rejecting move_hop: destination not reachable for {} (los={})",
                player->GetName(),
                hasLOS ? "yes" : "no"
            );
            return;
        }
        if (!hasLOS)
        {
            // LOS is not required for travel (pathfinding can route around), but is useful signal.
            LOG_DEBUG("server.loading", "[OllamaBotAmigo] move_hop destination lacks LOS for {}", player->GetName());
        }
        if (!movement->StartPathMove(player, dest, MoveReason::Travel))
        {
            LOG_INFO("server.loading", "[OllamaBotAmigo] move_hop path start failed for {}", player->GetName());
            return;
        }

        // Record semantic travel target (arrival radius + timeout) for downstream reporting.
        // Timeout is based on the current distance to destination, clamped to prevent indefinite wandering.
        uint32 nowMs = getMSTime();
        float dist = WorldPosition(player).distance(dest);
        float capped = std::min(dist, kMaxMoveDistanceCap);
        uint32 timeoutMs = static_cast<uint32>(std::clamp(capped * 1800.0f, 30000.0f, 180000.0f));
        std::ostringstream travelKey;
        travelKey << "move_hop:candidate:" << actionState.action.navEpoch << ":" << actionState.action.navCandidateId;
        AmigoTravelTarget targetSpec{travelKey.str(), dest, 2.5f, timeoutMs};
        travel->Begin(targetSpec, nowMs);
        return;
    }

    if (actionState.action.capability == ControlAction::Capability::Idle)
    {
        // Idle is logged but produces no Playerbot command.
        LOG_INFO(
            "server.loading",
            "[OllamaBotAmigo] Capability received for {}: {} reasoning='{}'",
            player->GetName(),
            CapabilityName(actionState.action.capability),
            actionState.reasoning
        );
        return;
    }

    if (actionState.action.capability == ControlAction::Capability::Fish)
    {
        if (snapshot.isMoving)
        {
            LOG_INFO("server.loading", "[OllamaBotAmigo] Rejecting fish: bot is moving for {}", player->GetName());
            return;
        }

        BotTravel* travel = BotTravelRegistry::Get(guid);
        if (travel && travel->Active())
        {
            LOG_INFO("server.loading", "[OllamaBotAmigo] Rejecting fish: travel already active for {}", player->GetName());
            return;
        }

        BotProfession* prof = BotProfessionRegistry::Get(guid);
        if (!prof)
        {
            LOG_INFO("server.loading", "[OllamaBotAmigo] No profession instance registered for {}", player->GetName());
            return;
        }
        if (prof->Active())
        {
            LOG_INFO("server.loading", "[OllamaBotAmigo] Rejecting fish: profession already active for {}", player->GetName());
            return;
        }

        uint32 nowMs = getMSTime();
        if (!prof->StartFishing(player, ai, nowMs))
        {
            LOG_INFO("server.loading", "[OllamaBotAmigo] Fish start failed for {}", player->GetName());
            return;
        }

        LOG_INFO(
            "server.loading",
            "[OllamaBotAmigo] Capability received for {}: {} reasoning='{}'",
            player->GetName(),
            CapabilityName(actionState.action.capability),
            actionState.reasoning
        );
        return;
    }

    if (actionState.action.capability == ControlAction::Capability::UseProfession)
    {
        uint32 skillId = 0;
        if (!TryMapSkill(actionState.action.professionSkill, skillId))
        {
            LOG_INFO("server.loading", "[OllamaBotAmigo] Rejecting profession: unknown skill '{}' for {}", actionState.action.professionSkill, player->GetName());
            return;
        }

        uint32 skillValue = player->GetSkillValue(skillId);
        if (skillValue == 0)
        {
            LOG_INFO("server.loading", "[OllamaBotAmigo] Rejecting profession: bot lacks skill '{}' for {}", actionState.action.professionSkill, player->GetName());
            return;
        }

        std::string intent = NormalizeToken(actionState.action.professionIntent);
        if (skillId == SKILL_FISHING && intent == "fish")
        {
            ControlActionState forwarded = actionState;
            forwarded.action.capability = ControlAction::Capability::Fish;
            forwarded.action.professionSkill.clear();
            forwarded.action.professionIntent.clear();
            ControlActionRegistry::Instance().Enqueue(guid, forwarded);
            return;
        }

        LOG_INFO(
            "server.loading",
            "[OllamaBotAmigo] Rejecting profession request for {}: skill='{}' intent='{}' (not implemented yet)",
            player->GetName(),
            actionState.action.professionSkill,
            actionState.action.professionIntent
        );
        return;
    }

    BotControlCommand command;
    std::string commandText;
    if (!ResolveCapabilityCommand(actionState.action.capability, command, commandText))
    {
        LOG_INFO("server.loading", "[OllamaBotAmigo] Rejecting unsupported capability for {}", player->GetName());
        return;
    }

    if (actionState.action.capability == ControlAction::Capability::TalkToQuestGiver && actionState.action.questId != 0)
    {
        WorldObject* questGiver = FindBestQuestGiverForQuestId(player, ai, actionState.action.questId);
        if (questGiver)
        {
            ObjectGuid qg = questGiver->GetGUID();
            if (questGiver->ToCreature())
            {
                player->SetSelection(qg);
            }
            else
            {
                player->SetTarget(qg);
            }

            PendingQuestGiverFollowup pending;
            pending.questId = actionState.action.questId;
            pending.questGiverGuid = qg;
            pending.startedMs = getMSTime();
            pending.initialStatus = player->GetQuestStatus(actionState.action.questId);
            pending.initialRewarded = player->GetQuestRewardStatus(actionState.action.questId);
            pendingQuestGiverFollowups[guid] = pending;
        }
    }

    LOG_INFO(
        "server.loading",
        "[OllamaBotAmigo] Capability resolved for {}: capability={} command='{}' reasoning='{}'",
        player->GetName(),
        CapabilityName(actionState.action.capability),
        commandText,
        actionState.reasoning
    );

    EnqueueBotControlCommand(player, command, actionState.reasoning);
}
