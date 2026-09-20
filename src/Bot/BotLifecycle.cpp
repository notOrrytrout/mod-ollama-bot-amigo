#include "Bot/BotLifecycle.h"

#include "Bot/BotControlApi.h"
#include "Bot/BotMovement.h"
#include "Bot/BotTravel.h"
#include "Util/PlayerbotsCompat.h"
#include "Playerbots.h"
#include "ChooseTravelTargetAction.h"
#include "TravelMgr.h"
#include "PlayerbotFactory.h"
#include "BudgetValues.h"
#include "LootObjectStack.h"
#include "Trainer.h"
#include "Creature.h"
#include "Log.h"
#include "SharedDefines.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr uint8 kUrgentBagSpacePct = 95;
    constexpr uint8 kUrgentDurabilityPct = 25;
    constexpr uint32 kLowFoodCount = 2;
    constexpr uint32 kLowDrinkCount = 2;
    constexpr uint32 kLowHunterAmmoCount = 100;
    constexpr uint32 kRetargetDelayMs = 5000;
    constexpr uint32 kServiceRetryDelayMs = 1500;
    constexpr uint32 kOverlayTimeoutMs = 10 * 60 * 1000;
    constexpr uint32 kMaintenanceRetryBackoffMs = 60 * 1000;
    constexpr uint32 kSupplyServiceFailureTimeoutMs = 20 * 1000;

    struct LifecycleRuntime
    {
        bool active = false;
        BotMaintenanceKind requested = BotMaintenanceKind::None;
        BotMaintenanceKind kind = BotMaintenanceKind::None;
        uint64 missionRevisionAtStart = 0;
        uint32 startedMs = 0;
        uint32 lastTravelSelectMs = 0;
        uint32 lastServiceAttemptMs = 0;
        uint32 retryAfterMs = 0;
        uint32 supplyFailureSinceMs = 0;
        uint32 supplyFoodCount = 0;
        uint32 supplyDrinkCount = 0;
        uint32 supplyAmmoCount = 0;
        uint32 supplyLastProgressMs = 0;
        bool travelWasEnabled = false;
        bool rpgWasEnabled = false;
        bool grindWasEnabled = false;
        bool pausedForPreemption = false;
        uint32 lastObservedLevel = 0;
        bool trainingDue = false;
        uint64 generation = 1;
        BotLifecycleLane observedLane = BotLifecycleLane::Mission;
    };

    std::mutex g_lifecycleMutex;
    std::unordered_map<uint64, LifecycleRuntime> g_lifecycle;

    uint64 BotGuid(Player const* bot)
    {
        return bot ? bot->GetGUID().GetRawValue() : 0;
    }

    template <class T>
    bool TryReadValue(PlayerbotAI* ai, std::string const& name, T& out)
    {
        if (!ai || !ai->GetAiObjectContext())
            return false;

        if (Value<T>* value = ai->GetAiObjectContext()->GetValue<T>(name))
        {
            out = value->Get();
            return true;
        }
        return false;
    }

    template <class T>
    bool TryReadValue(PlayerbotAI* ai, std::string const& name, std::string const& qualifier, T& out)
    {
        if (!ai || !ai->GetAiObjectContext())
            return false;

        if (Value<T>* value = ai->GetAiObjectContext()->GetValue<T>(name, qualifier))
        {
            out = value->Get();
            return true;
        }
        return false;
    }

    bool TrainerHasAffordableLearnableSpell(Player* player, PlayerbotAI* ai, uint32 trainerEntry)
    {
        if (!player || !ai || !trainerEntry)
            return false;

        Trainer::Trainer* trainer = sObjectMgr->GetTrainer(trainerEntry);
        if (!trainer || !trainer->IsTrainerValidForPlayer(player))
            return false;

        CreatureTemplate const* creatureInfo = sObjectMgr->GetCreatureTemplate(trainerEntry);
        if (!creatureInfo)
            return false;

        FactionTemplateEntry const* factionTemplate = sFactionTemplateStore.LookupEntry(creatureInfo->faction);
        const float reputationDiscount = factionTemplate ? player->GetReputationPriceDiscount(factionTemplate) : 1.0f;

        uint32 freeMoney = player->GetMoney();
        TryReadValue(ai,
                     "free money for",
                     std::to_string(static_cast<uint32>(NeedMoneyFor::spells)),
                     freeMoney);

        for (Trainer::Spell const& spell : trainer->GetSpells())
        {
            Trainer::Spell const* trainerSpell = trainer->GetSpell(spell.SpellId);
            if (!trainerSpell)
                continue;
            if (!PlayerbotFactory::IsTrainerSpellAllowedForBot(player, trainer, trainerSpell))
                continue;
            if (!trainer->CanTeachSpell(player, trainerSpell))
                continue;

            const uint32 cost = static_cast<uint32>(std::floor(trainerSpell->MoneyCost * reputationDiscount));
            if (freeMoney >= cost)
                return true;
        }

        return false;
    }

    class AmigoServiceTravelSelector : public ChooseTravelTargetAction
    {
    public:
        explicit AmigoServiceTravelSelector(PlayerbotAI* ai)
            : ChooseTravelTargetAction(ai, "amigo service travel")
        {
        }

        bool Select(std::vector<NPCFlags> const& flags, bool apply = true)
        {
            if (!botAI || !botAI->GetAiObjectContext())
                return false;

            TravelTarget* oldTarget = botAI->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
            if (!oldTarget)
                return false;

            TravelTarget nextTarget(botAI);
            if (!SetNpcFlagTarget(&nextTarget, flags))
                return false;
            if (!nextTarget.getPosition() || nextTarget.getPosition()->GetMapId() != botAI->GetBot()->GetMapId())
                return false;
            if (apply)
                setNewTarget(&nextTarget, oldTarget);
            return true;
        }

        bool SelectTrainerWithLearnableSpell(bool apply = true)
        {
            if (!botAI || !botAI->GetAiObjectContext() || !botAI->GetBot())
                return false;

            Player* player = botAI->GetBot();
            TravelTarget* oldTarget = botAI->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
            if (!oldTarget)
                return false;

            WorldPosition botPos(player);
            std::vector<TravelDestination*> candidates;
            for (TravelDestination* destination : TravelMgr::instance().getRpgTravelDestinations(player, true, true))
            {
                if (!destination || !destination->getEntry())
                    continue;

                CreatureTemplate const* creatureInfo = sObjectMgr->GetCreatureTemplate(destination->getEntry());
                if (!creatureInfo || !(creatureInfo->npcflag & UNIT_NPC_FLAG_TRAINER))
                    continue;

                if (!TrainerHasAffordableLearnableSpell(player, botAI, destination->getEntry()))
                    continue;

                FactionTemplateEntry const* factionEntry = sFactionTemplateStore.LookupEntry(creatureInfo->faction);
                if (!factionEntry ||
                    Unit::GetFactionReactionTo(player->GetFactionTemplateEntry(), factionEntry) < REP_NEUTRAL)
                    continue;

                candidates.push_back(destination);
            }

            if (candidates.empty())
                return false;

            TravelDestination* destination = *std::min_element(
                candidates.begin(), candidates.end(),
                [&botPos](TravelDestination* lhs, TravelDestination* rhs)
                {
                    return lhs->distanceTo(const_cast<WorldPosition*>(&botPos)) <
                           rhs->distanceTo(const_cast<WorldPosition*>(&botPos));
                });

            std::vector<WorldPosition*> points = destination->nextPoint(const_cast<WorldPosition*>(&botPos), true);
            if (points.empty() || points.front()->GetMapId() != player->GetMapId())
                return false;

            TravelTarget nextTarget(botAI);
            nextTarget.setTarget(destination, points.front());
            nextTarget.setForced(true);
            if (apply)
                setNewTarget(&nextTarget, oldTarget);
            return true;
        }
    };

    Creature* FindNearestServiceNpc(Player* bot, PlayerbotAI* ai, BotMaintenanceKind kind)
    {
        if (!bot || !ai || !ai->GetAiObjectContext())
            return nullptr;

        NPCFlags requiredFlag = UNIT_NPC_FLAG_NONE;
        switch (kind)
        {
            case BotMaintenanceKind::BagSpace:
            case BotMaintenanceKind::Supplies:
                requiredFlag = UNIT_NPC_FLAG_VENDOR;
                break;
            case BotMaintenanceKind::Repair:
                requiredFlag = UNIT_NPC_FLAG_REPAIR;
                break;
            case BotMaintenanceKind::Training:
                requiredFlag = UNIT_NPC_FLAG_TRAINER;
                break;
            case BotMaintenanceKind::None:
            default:
                return nullptr;
        }

        Creature* best = nullptr;
        float bestDistance = 0.0f;
        GuidVector npcs = ai->GetAiObjectContext()->GetValue<GuidVector>("nearest npcs")->Get();
        for (ObjectGuid const& guid : npcs)
        {
            // Use the same interaction gate as Playerbots service actions. A
            // creature can be in "nearest npcs" while still being too far away.
            Creature* creature = bot->GetNPCIfCanInteractWith(guid, requiredFlag);
            if (!creature || !creature->IsAlive())
                continue;
            if (kind == BotMaintenanceKind::Training &&
                !TrainerHasAffordableLearnableSpell(bot, ai, creature->GetEntry()))
                continue;

            float distance = bot->GetDistance(creature);
            if (!best || distance < bestDistance)
            {
                best = creature;
                bestDistance = distance;
            }
        }
        return best;
    }

    std::vector<NPCFlags> ServiceFlags(BotMaintenanceKind kind)
    {
        switch (kind)
        {
            case BotMaintenanceKind::BagSpace:
            case BotMaintenanceKind::Supplies:
                return {UNIT_NPC_FLAG_VENDOR};
            case BotMaintenanceKind::Repair:
                return {UNIT_NPC_FLAG_REPAIR};
            case BotMaintenanceKind::Training:
                return {UNIT_NPC_FLAG_TRAINER};
            case BotMaintenanceKind::None:
            default:
                return {};
        }
    }

    void ApplyOverlayStrategies(PlayerbotAI* ai, LifecycleRuntime& runtime)
    {
        if (!ai)
            return;

        runtime.travelWasEnabled = ai->HasStrategy("travel", BOT_STATE_NON_COMBAT);
        runtime.rpgWasEnabled = ai->HasStrategy("rpg", BOT_STATE_NON_COMBAT);
        runtime.grindWasEnabled = ai->HasStrategy("grind", BOT_STATE_NON_COMBAT);

        if (!runtime.travelWasEnabled)
            ai->ChangeStrategy("+travel", BOT_STATE_NON_COMBAT);
        if (!runtime.rpgWasEnabled)
            ai->ChangeStrategy("+rpg", BOT_STATE_NON_COMBAT);
        if (runtime.grindWasEnabled)
            ai->ChangeStrategy("-grind", BOT_STATE_NON_COMBAT);
    }

    void PauseOverlayForDeterministicLane(PlayerbotAI* ai, LifecycleRuntime& runtime)
    {
        if (!ai || !runtime.active || runtime.pausedForPreemption)
            return;

        // Needs must be able to stop and eat/drink without the maintenance
        // travel/RPG strategies immediately moving the bot again. Keep the
        // retained destination and overlay state; only suspend execution.
        if (!runtime.travelWasEnabled)
            ai->ChangeStrategy("-travel", BOT_STATE_NON_COMBAT);
        if (!runtime.rpgWasEnabled)
            ai->ChangeStrategy("-rpg", BOT_STATE_NON_COMBAT);
        runtime.pausedForPreemption = true;
    }

    void ResumeOverlayAfterPreemption(PlayerbotAI* ai, LifecycleRuntime& runtime)
    {
        if (!ai || !runtime.active || !runtime.pausedForPreemption)
            return;

        if (!runtime.travelWasEnabled)
            ai->ChangeStrategy("+travel", BOT_STATE_NON_COMBAT);
        if (!runtime.rpgWasEnabled)
            ai->ChangeStrategy("+rpg", BOT_STATE_NON_COMBAT);
        runtime.pausedForPreemption = false;
    }

    void ReleaseOverlay(Player* bot, PlayerbotAI* ai, LifecycleRuntime& runtime, uint64 currentMissionRevision, char const* reason)
    {
        if (ai)
        {
            if (!runtime.travelWasEnabled)
                ai->ChangeStrategy("-travel", BOT_STATE_NON_COMBAT);
            if (!runtime.rpgWasEnabled)
                ai->ChangeStrategy("-rpg", BOT_STATE_NON_COMBAT);

            // Restore a mission-owned grind strategy only when the durable mission
            // did not change while maintenance held the lane.
            if (runtime.grindWasEnabled && runtime.missionRevisionAtStart == currentMissionRevision)
                ai->ChangeStrategy("+grind", BOT_STATE_NON_COMBAT);
        }

        if (bot)
        {
            UpdateActivityState(bot, "mission", reason ? reason : "maintenance_complete");
            LOG_INFO("server.loading",
                     "[OllamaBotAmigo] Lifecycle maintenance released for {}: kind={} reason={} mission_revision_start={} mission_revision_now={}",
                     bot->GetName(),
                     static_cast<int>(runtime.kind),
                     reason ? reason : "complete",
                     runtime.missionRevisionAtStart,
                     currentMissionRevision);
        }

        const uint32 observedLevel = runtime.lastObservedLevel;
        const bool trainingDue = runtime.trainingDue;
        const uint64 generation = runtime.generation;
        const BotLifecycleLane observedLane = runtime.observedLane;
        const uint32 retryAfterMs = runtime.retryAfterMs;
        runtime = LifecycleRuntime{};
        runtime.lastObservedLevel = observedLevel;
        runtime.trainingDue = trainingDue;
        runtime.generation = generation;
        runtime.observedLane = observedLane;
        runtime.retryAfterMs = retryAfterMs;
    }

    bool PerformLocalService(Player* bot, PlayerbotAI* ai, LifecycleRuntime& runtime)
    {
        Creature* serviceNpc = FindNearestServiceNpc(bot, ai, runtime.kind);
        if (!serviceNpc)
            return false;

        bot->SetSelection(serviceNpc->GetGUID());

        bool started = false;
        switch (runtime.kind)
        {
            case BotMaintenanceKind::BagSpace:
                started = ExecutePlayerbotAction(bot, "sell",
                    runtime.requested == BotMaintenanceKind::BagSpace ? "gray" : "vendor");
                // Let Playerbots opportunistically restock useful items while the
                // vendor is already selected. Failure is harmless and does not
                // change the sell result.
                if (runtime.requested == BotMaintenanceKind::None)
                    ExecutePlayerbotAction(bot, "buy", "vendor");
                break;
            case BotMaintenanceKind::Repair:
                started = ExecutePlayerbotAction(bot, "repair");
                break;
            case BotMaintenanceKind::Supplies:
                started = ExecutePlayerbotAction(bot, "buy", "vendor");
                if (!started)
                    started = ExecutePlayerbotAction(bot, "maintenance");
                break;
            case BotMaintenanceKind::Training:
                started = ExecutePlayerbotAction(bot, "trainer", "learn");
                break;
            case BotMaintenanceKind::None:
            default:
                break;
        }

        return started;
    }
}

std::string BotLifecycleAssessment::LaneName() const
{
    switch (lane)
    {
        case BotLifecycleLane::Combat: return "combat";
        case BotLifecycleLane::Needs: return "needs";
        case BotLifecycleLane::Loot: return "loot";
        case BotLifecycleLane::Maintenance: return "maintenance";
        case BotLifecycleLane::Mission:
        default: return "mission";
    }
}

std::string BotLifecycleAssessment::MaintenanceName() const
{
    switch (maintenance)
    {
        case BotMaintenanceKind::BagSpace: return "bag_space";
        case BotMaintenanceKind::Repair: return "repair";
        case BotMaintenanceKind::Supplies: return "supplies";
        case BotMaintenanceKind::Training: return "training";
        case BotMaintenanceKind::None:
        default: return "none";
    }
}

BotLifecycleAssessment AssessBotLifecycle(Player const* bot, PlayerbotAI* ai, BotNeedAssessment const& topNeed)
{
    BotLifecycleAssessment out;

    if (topNeed.kind != BotNeedKind::None)
    {
        out.lane = BotLifecycleLane::Needs;
        out.urgent = topNeed.urgent;
        out.score = topNeed.score;
        out.reason = topNeed.reason;
        return out;
    }

    if (!bot || !ai || !bot->IsAlive())
        return out;

    // Once combat has started, Playerbots owns the fight. The LLM chooses the
    // engagement target, but it does not steer rotations or movement mid-fight.
    if (bot->IsInCombat())
    {
        out.lane = BotLifecycleLane::Combat;
        out.score = 900;
        out.reason = "Playerbots combat owns execution";
        return out;
    }

    // Corpse/object loot is deterministic Playerbots work. Use its own
    // available-loot value instead of asking the LLM whether it should loot.
    bool canLoot = false;
    TryReadValue(ai, "can loot", canLoot);
    LootObjectStack* availableLoot = nullptr;
    TryReadValue(ai, "available loot", availableLoot);
    bool hasPendingLoot = availableLoot && !availableLoot->GetLoot().IsEmpty();
    // `has available loot` can remain true while Playerbots is still moving
    // toward a corpse or while the corpse is no longer openable. The stack
    // and the direct can-loot value identify actionable loot more accurately.
    if (canLoot || hasPendingLoot)
    {
        out.lane = BotLifecycleLane::Loot;
        out.score = 700;
        out.reason = canLoot ? "Playerbots can open current loot target"
                             : "Playerbots has pending loot target";
        return out;
    }

    TryReadValue(ai, "bag space", out.bagSpacePct);
    TryReadValue(ai, "durability", out.durabilityPct);
    TryReadValue(ai, "should sell", out.shouldSell);
    TryReadValue(ai, "can sell", out.canSell);
    TryReadValue(ai, "should repair", out.shouldRepair);
    TryReadValue(ai, "can repair", out.canRepair);
    TryReadValue(ai, "item count", "food", out.foodCount);
    TryReadValue(ai, "item count", "drink", out.drinkCount);
    TryReadValue(ai, "item count", "ammo", out.ammoCount);

    {
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        auto it = g_lifecycle.find(BotGuid(bot));
        if (it != g_lifecycle.end() && it->second.requested != BotMaintenanceKind::None)
        {
            out.lane = BotLifecycleLane::Maintenance;
            out.maintenance = it->second.requested;
            out.score = 600;
            out.reason = "requested service: travel and interact";
            return out;
        }
    }

    struct Candidate
    {
        BotMaintenanceKind kind;
        uint16 score;
        bool urgent;
        std::string reason;
    };
    std::vector<Candidate> candidates;

    if (out.shouldSell && out.canSell)
    {
        const bool urgent = out.bagSpacePct >= kUrgentBagSpacePct;
        std::ostringstream reason;
        reason << "playerbots bag space is " << static_cast<uint32>(out.bagSpacePct) << "% used";
        candidates.push_back({BotMaintenanceKind::BagSpace, static_cast<uint16>(urgent ? 850 : 520), urgent, reason.str()});
    }

    if (out.shouldRepair && out.canRepair)
    {
        const bool urgent = out.durabilityPct <= kUrgentDurabilityPct;
        std::ostringstream reason;
        reason << "playerbots durability is " << static_cast<uint32>(out.durabilityPct) << "%";
        candidates.push_back({BotMaintenanceKind::Repair, static_cast<uint16>(urgent ? 820 : 500), urgent, reason.str()});
    }

    const bool usesMana = bot->GetMaxPower(POWER_MANA) > 0;
    const bool needsFood = out.foodCount < kLowFoodCount;
    const bool needsDrink = usesMana && out.drinkCount < kLowDrinkCount;
    const bool needsAmmo = bot->getClass() == CLASS_HUNTER && out.ammoCount < kLowHunterAmmoCount;
    if (needsFood || needsDrink || needsAmmo)
    {
        // Food/drink shortages are maintenance preferences, not hard lane-locking
        // emergencies: characters can naturally regenerate and Playerbots may not
        // have a compatible vendor/item available. Zero hunter ammo is different
        // because it can directly prevent normal ranged combat.
        const bool urgent = bot->getClass() == CLASS_HUNTER && out.ammoCount == 0;
        std::ostringstream reason;
        reason << "low Playerbots supplies: food=" << out.foodCount;
        if (usesMana)
            reason << " drink=" << out.drinkCount;
        if (bot->getClass() == CLASS_HUNTER)
            reason << " ammo=" << out.ammoCount;
        candidates.push_back({BotMaintenanceKind::Supplies, static_cast<uint16>(urgent ? 610 : 360), urgent, reason.str()});
    }

    // Training is intentionally event-driven. Playerbots' generic "can train"
    // value is a money check, not proof that the character has a learnable spell.
    // Mark training due after an observed level increase and let TrainerAction do
    // the authoritative isPossible() check at the trainer.
    {
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        LifecycleRuntime& runtime = g_lifecycle[BotGuid(bot)];
        const uint32 level = bot->GetLevel();
        if (runtime.lastObservedLevel == 0)
            runtime.lastObservedLevel = level;
        else if (level > runtime.lastObservedLevel)
        {
            runtime.lastObservedLevel = level;
            runtime.trainingDue = true;
        }

        if (runtime.trainingDue)
        {
            candidates.push_back({BotMaintenanceKind::Training, 300, false, "level increased; trainer check is due"});
        }
    }

    if (candidates.empty())
        return out;

    auto best = std::max_element(candidates.begin(), candidates.end(), [](Candidate const& lhs, Candidate const& rhs)
    {
        return lhs.score < rhs.score;
    });

    out.lane = BotLifecycleLane::Maintenance;
    out.maintenance = best->kind;
    out.score = best->score;
    out.urgent = best->urgent;
    out.reason = best->reason;
    return out;
}

bool HasActiveBotLifecycleOverlay(Player const* bot)
{
    if (!bot)
        return false;
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    auto it = g_lifecycle.find(BotGuid(bot));
    return it != g_lifecycle.end() && it->second.active;
}

bool CanAcquireBotLifecycleMaintenance(Player const* bot, bool /*urgent*/)
{
    if (!bot)
        return false;

    // Retry backoff must apply even to urgent maintenance. Otherwise an urgent
    // condition such as food=0/drink=0 immediately reacquires the maintenance
    // lane every tick after a failed service attempt and can pin the bot away
    // from its durable mission indefinitely.
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    auto it = g_lifecycle.find(BotGuid(bot));
    if (it == g_lifecycle.end() || it->second.active || it->second.retryAfterMs == 0)
        return true;
    return getMSTime() >= it->second.retryAfterMs;
}

uint64 GetBotLifecycleGeneration(uint64 botGuid)
{
    if (!botGuid)
        return 0;
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    return g_lifecycle[botGuid].generation;
}

uint64 GetBotLifecycleGeneration(Player const* bot)
{
    return bot ? GetBotLifecycleGeneration(BotGuid(bot)) : 0;
}

uint64 SynchronizeBotLifecycleLane(Player const* bot, BotLifecycleLane lane)
{
    if (!bot)
        return 0;

    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    LifecycleRuntime& runtime = g_lifecycle[BotGuid(bot)];
    if (runtime.observedLane != lane)
    {
        runtime.observedLane = lane;
        ++runtime.generation;
        if (runtime.generation == 0)
            runtime.generation = 1;
    }
    return runtime.generation;
}

bool RequestBotLifecycleService(Player* bot, BotMaintenanceKind kind)
{
    std::string reason;
    return RequestBotLifecycleService(bot, kind, reason);
}

BotServiceAvailability AssessBotServiceAvailability(Player* bot, PlayerbotAI* ai, BotMaintenanceKind kind)
{
    BotServiceAvailability out;
    if (!bot || !ai || !bot->IsAlive() || kind == BotMaintenanceKind::None)
    {
        out.reason = "service_invalid_for_bot";
        return out;
    }
    uint32 count = 0;
    uint8 durability = 100;
    if (kind == BotMaintenanceKind::BagSpace)
        out.needed = TryReadValue(ai, "item count", "gray", count) && count > 0;
    else if (kind == BotMaintenanceKind::Repair)
        out.needed = TryReadValue(ai, "durability", durability) && durability < 100;
    else if (kind == BotMaintenanceKind::Training)
        out.needed = true; // The trainer selection checks eligibility and cost.
    else if (kind == BotMaintenanceKind::Supplies)
    {
        out.needed = TryReadValue(ai, "item count", "food", count) && count < kLowFoodCount;
        if (bot->GetMaxPower(POWER_MANA))
            out.needed = (TryReadValue(ai, "item count", "drink", count) && count < kLowDrinkCount) || out.needed;
        if (bot->getClass() == CLASS_HUNTER)
            out.needed = (TryReadValue(ai, "item count", "ammo", count) && count < kLowHunterAmmoCount) || out.needed;
    }
    if (!out.needed)
    {
        out.reason = "service_not_needed";
        return out;
    }
    out.local = FindNearestServiceNpc(bot, ai, kind) != nullptr;
    if (!out.local)
    {
        AmigoServiceTravelSelector selector(ai);
        out.travel = kind == BotMaintenanceKind::Training ? selector.SelectTrainerWithLearnableSpell(false) :
            selector.Select(ServiceFlags(kind), false);
    }
    if (out.local || out.travel)
        out.reason = out.local ? "service_available_locally" : "service_available_through_travel";
    return out;
}

bool RequestBotLifecycleService(Player* bot, BotMaintenanceKind kind, std::string& reason)
{
    reason.clear();
    if (!bot || kind == BotMaintenanceKind::None)
    {
        reason = "service_invalid_for_bot";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    LifecycleRuntime& runtime = g_lifecycle[BotGuid(bot)];
    if (runtime.active)
    {
        reason = runtime.kind == kind ? "service_busy_same_kind" : "service_busy";
        return false;
    }
    if (runtime.retryAfterMs && getMSTime() < runtime.retryAfterMs)
    {
        reason = "service_backoff";
        return false;
    }
    if (runtime.requested != BotMaintenanceKind::None)
    {
        if (runtime.requested == kind)
        {
            reason = "service_already_queued";
            return true;
        }
        reason = "service_busy";
        return false;
    }
    PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
    auto availability = AssessBotServiceAvailability(bot, ai, kind);
    if (!availability.local && !availability.travel)
    {
        reason = availability.reason;
        return false;
    }
    runtime.requested = kind;
    if (auto* movement = BotMovementRegistry::Get(BotGuid(bot)))
        movement->Abort(MoveReason::Travel);
    if (auto* travel = BotTravelRegistry::Get(BotGuid(bot)))
        travel->Abort(getMSTime());
    bot->StopMoving();
    reason = "service_queued_for_lifecycle_travel";
    return true;
}

bool HasPendingBotLifecycleService(Player const* bot)
{
    if (!bot)
        return false;

    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    auto it = g_lifecycle.find(BotGuid(bot));
    return it != g_lifecycle.end() && it->second.requested != BotMaintenanceKind::None;
}

bool ServiceBotLifecycle(Player* bot,
                         PlayerbotAI* ai,
                         BotLifecycleAssessment const& assessment,
                         uint64 missionRevision,
                         bool allowOptionalStart)
{
    if (!bot || !ai)
        return false;

    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    LifecycleRuntime& runtime = g_lifecycle[BotGuid(bot)];

    // Higher-priority deterministic lanes can preempt a retained maintenance
    // route without destroying it. Suspend travel/RPG execution and resume it
    // when maintenance becomes the owner again.
    if (assessment.lane == BotLifecycleLane::Needs ||
        assessment.lane == BotLifecycleLane::Combat ||
        assessment.lane == BotLifecycleLane::Loot)
    {
        if (runtime.active)
            PauseOverlayForDeterministicLane(ai, runtime);
        return false;
    }

    if (assessment.lane != BotLifecycleLane::Maintenance)
    {
        if (runtime.active)
            ReleaseOverlay(bot, ai, runtime, missionRevision, "maintenance_condition_cleared");
        return false;
    }

    const uint32 nowMs = getMSTime();
    if (runtime.requested != BotMaintenanceKind::None && runtime.active &&
        runtime.missionRevisionAtStart != missionRevision)
    {
        ReleaseOverlay(bot, ai, runtime, missionRevision, "service_mission_changed");
        return false;
    }
    if (runtime.active)
        ResumeOverlayAfterPreemption(ai, runtime);

    if (!runtime.active && runtime.retryAfterMs != 0 && nowMs < runtime.retryAfterMs)
        return false;
    if (!runtime.active && !assessment.urgent && !allowOptionalStart && runtime.requested == BotMaintenanceKind::None)
        return false;

    if (!runtime.active)
    {
        runtime.retryAfterMs = 0;
        runtime.active = true;
        runtime.kind = assessment.maintenance;
        runtime.missionRevisionAtStart = missionRevision;
        runtime.startedMs = nowMs;
        runtime.supplyFailureSinceMs = 0;
        runtime.supplyFoodCount = assessment.foodCount;
        runtime.supplyDrinkCount = assessment.drinkCount;
        runtime.supplyAmmoCount = assessment.ammoCount;
        runtime.supplyLastProgressMs = nowMs;
        ApplyOverlayStrategies(ai, runtime);
        UpdateActivityState(bot, "maintenance", assessment.reason);
        LOG_INFO("server.loading",
                 "[OllamaBotAmigo] Lifecycle maintenance acquired for {}: kind={} urgent={} reason='{}' mission_revision={}",
                 bot->GetName(),
                 assessment.MaintenanceName(),
                 assessment.urgent ? "yes" : "no",
                 assessment.reason,
                 missionRevision);
    }
    else if (runtime.kind != assessment.maintenance)
    {
        // Keep one retained overlay but allow the highest-scoring maintenance
        // need to change as the first service is completed.
        runtime.kind = assessment.maintenance;
        runtime.lastTravelSelectMs = 0;
        runtime.lastServiceAttemptMs = 0;
        runtime.supplyFailureSinceMs = 0;
        runtime.supplyFoodCount = assessment.foodCount;
        runtime.supplyDrinkCount = assessment.drinkCount;
        runtime.supplyAmmoCount = assessment.ammoCount;
        runtime.supplyLastProgressMs = nowMs;
        UpdateActivityState(bot, "maintenance", assessment.reason);
    }

    if (runtime.active && runtime.kind == BotMaintenanceKind::Supplies)
    {
        const bool improved = assessment.foodCount > runtime.supplyFoodCount ||
                              assessment.drinkCount > runtime.supplyDrinkCount ||
                              assessment.ammoCount > runtime.supplyAmmoCount;
        if (improved)
        {
            if (runtime.requested == BotMaintenanceKind::Supplies)
            {
                ReleaseOverlay(bot, ai, runtime, missionRevision, "supplies_inventory_progress_verified");
                return true;
            }
            runtime.supplyFoodCount = assessment.foodCount;
            runtime.supplyDrinkCount = assessment.drinkCount;
            runtime.supplyAmmoCount = assessment.ammoCount;
            runtime.supplyLastProgressMs = nowMs;
        }
        else if (runtime.lastServiceAttemptMs != 0 && runtime.supplyLastProgressMs != 0 &&
                 nowMs - runtime.supplyLastProgressMs >= kSupplyServiceFailureTimeoutMs)
        {
            runtime.retryAfterMs = nowMs + kMaintenanceRetryBackoffMs;
            runtime.observedLane = BotLifecycleLane::Mission;
            ++runtime.generation;
            if (runtime.generation == 0)
                runtime.generation = 1;
            ReleaseOverlay(bot, ai, runtime, missionRevision, "supplies_no_inventory_progress");
            return true;
        }
    }

    if (runtime.startedMs != 0 && nowMs - runtime.startedMs > kOverlayTimeoutMs)
    {
        runtime.retryAfterMs = nowMs + kMaintenanceRetryBackoffMs;
        runtime.observedLane = BotLifecycleLane::Mission;
        ++runtime.generation;
        if (runtime.generation == 0)
            runtime.generation = 1;
        ReleaseOverlay(bot, ai, runtime, missionRevision, "maintenance_timeout");
        return true;
    }

    if (bot->IsInCombat())
        return true;

    if (Creature* serviceNpc = FindNearestServiceNpc(bot, ai, runtime.kind))
    {
        (void)serviceNpc;
        if (runtime.lastServiceAttemptMs == 0 || nowMs - runtime.lastServiceAttemptMs >= kServiceRetryDelayMs)
        {
            if (runtime.lastServiceAttemptMs == 0)
                runtime.supplyLastProgressMs = nowMs;
            runtime.lastServiceAttemptMs = nowMs;
            auto spellsBefore = bot->GetSpellMap().size();
            const bool serviceStarted = PerformLocalService(bot, ai, runtime);
            bool serviceComplete = false;
            if (runtime.requested == BotMaintenanceKind::BagSpace)
            {
                uint32 grayCount = 0;
                serviceComplete = TryReadValue(ai, "item count", "gray", grayCount) && grayCount == 0;
            }
            if (runtime.requested == BotMaintenanceKind::Repair)
            {
                uint8 durability = 0;
                serviceComplete = TryReadValue(ai, "durability", durability) && durability == 100;
            }
            if (runtime.kind == BotMaintenanceKind::Training)
            {
                serviceComplete = bot->GetSpellMap().size() > spellsBefore ||
                    !TrainerHasAffordableLearnableSpell(bot, ai, serviceNpc->GetEntry());
                if (serviceComplete)
                    runtime.trainingDue = false;
            }
            LOG_INFO("server.loading", "[OllamaBotAmigo] Service interaction for {}: started={} complete={}",
                     bot->GetName(), serviceStarted, serviceComplete);
            if ((runtime.requested != BotMaintenanceKind::None || runtime.kind == BotMaintenanceKind::Training) && serviceComplete)
            {
                ReleaseOverlay(bot, ai, runtime, missionRevision,
                               runtime.kind == BotMaintenanceKind::Training ? "training_completion_verified" : "requested_service_executed");
                return true;
            }

            // The Playerbots action return value only means the action ran; it is
            // not evidence that inventory supplies increased. Actual progress is
            // measured from the next lifecycle assessment above. Keep the boolean
            // only for diagnostics/failure handling of non-progressing actions.
            if (runtime.kind == BotMaintenanceKind::Supplies && !serviceStarted &&
                runtime.supplyFailureSinceMs == 0)
            {
                runtime.supplyFailureSinceMs = nowMs;
            }
        }
        return true;
    }

    if (runtime.lastTravelSelectMs == 0 || nowMs - runtime.lastTravelSelectMs >= kRetargetDelayMs)
    {
        runtime.lastTravelSelectMs = nowMs;
        std::vector<NPCFlags> flags = ServiceFlags(runtime.kind);
        if (!flags.empty())
        {
            AmigoServiceTravelSelector selector(ai);
            bool selected = runtime.kind == BotMaintenanceKind::Training
                                ? selector.SelectTrainerWithLearnableSpell()
                                : selector.Select(flags);
            if (selected)
            {
                LOG_INFO("server.loading",
                         "[OllamaBotAmigo] Playerbots service travel selected for {}: kind={} reason='{}'",
                         bot->GetName(),
                         assessment.MaintenanceName(),
                         assessment.reason);
            }
            else if (runtime.kind == BotMaintenanceKind::Supplies)
            {
                // Supply travel can fail when no compatible vendor is available.
                // Playerbots' maintenance action is the final native fallback. If
                // even that cannot start, release the lane and back off instead of
                // pinning the bot in urgent maintenance forever.
                if (!ExecutePlayerbotAction(bot, "maintenance"))
                {
                    runtime.retryAfterMs = nowMs + kMaintenanceRetryBackoffMs;
                    runtime.observedLane = BotLifecycleLane::Mission;
                    ++runtime.generation;
                    if (runtime.generation == 0)
                        runtime.generation = 1;
                    ReleaseOverlay(bot, ai, runtime, missionRevision, "no_supply_destination");
                    return true;
                }
            }
            else if (runtime.kind == BotMaintenanceKind::Training)
            {
                // Do not pin the mission to an invalid trainer search. The next
                // level increase will schedule another authoritative trainer check.
                runtime.trainingDue = false;
                runtime.observedLane = BotLifecycleLane::Mission;
                ++runtime.generation;
                if (runtime.generation == 0)
                    runtime.generation = 1;
                ReleaseOverlay(bot, ai, runtime, missionRevision, "no_learnable_trainer_destination");
                return true;
            }
        }
    }

    return true;
}

void ResetBotLifecycle(Player const* bot)
{
    if (!bot)
        return;

    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    const uint64 guid = BotGuid(bot);
    uint64 nextGeneration = 1;
    auto it = g_lifecycle.find(guid);
    if (it != g_lifecycle.end())
    {
        nextGeneration = it->second.generation + 1;
        if (nextGeneration == 0)
            nextGeneration = 1;
    }

    LifecycleRuntime fresh;
    fresh.generation = nextGeneration;
    g_lifecycle[guid] = fresh;
}
