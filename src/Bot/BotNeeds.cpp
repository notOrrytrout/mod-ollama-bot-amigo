#include "Bot/BotNeeds.h"
#include "Player.h"
#include "SharedDefines.h"

#include <algorithm>

std::string BotNeedAssessment::KindName() const
{
    switch (kind)
    {
        case BotNeedKind::Health: return "health";
        case BotNeedKind::Mana: return "mana";
        case BotNeedKind::Survival: return "survival";
        case BotNeedKind::None: default: return "none";
    }
}

BotNeedAssessment AssessBotNeeds(Player const* bot)
{
    BotNeedAssessment out;
    if (!bot || !bot->IsAlive())
    {
        out.kind = BotNeedKind::Survival;
        out.urgent = true;
        out.score = 1000;
        out.reason = "dead_or_unavailable";
        return out;
    }

    float hpPct = 100.0f;
    if (bot->GetMaxHealth() > 0)
    {
        hpPct = static_cast<float>(bot->GetHealth()) * 100.0f / static_cast<float>(bot->GetMaxHealth());
    }

    if (bot->IsInCombat() && hpPct <= 30.0f)
    {
        out.kind = BotNeedKind::Survival;
        out.urgent = true;
        out.score = static_cast<uint16>(900 + std::min(99.0f, 30.0f - hpPct));
        out.reason = "critical_health_in_combat";
        return out;
    }

    if (!bot->IsInCombat() && hpPct <= 45.0f)
    {
        out.kind = BotNeedKind::Health;
        out.urgent = hpPct <= 25.0f;
        out.score = static_cast<uint16>(700 + std::min(99.0f, 45.0f - hpPct));
        out.reason = out.urgent ? "critical_health_recovery" : "health_recovery";
        return out;
    }

    uint32 maxMana = bot->GetMaxPower(POWER_MANA);
    if (!bot->IsInCombat() && maxMana > 0)
    {
        float manaPct = static_cast<float>(bot->GetPower(POWER_MANA)) * 100.0f / static_cast<float>(maxMana);
        if (manaPct <= 30.0f)
        {
            out.kind = BotNeedKind::Mana;
            out.urgent = manaPct <= 12.0f;
            out.score = static_cast<uint16>(500 + std::min(99.0f, 30.0f - manaPct));
            out.reason = out.urgent ? "critical_mana_recovery" : "mana_recovery";
            return out;
        }
    }

    return out;
}
