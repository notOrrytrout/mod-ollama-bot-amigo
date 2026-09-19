#pragma once

#include "Define.h"
#include <string>

class Player;

enum class BotNeedKind : uint8
{
    None,
    Health,
    Mana,
    Survival
};

struct BotNeedAssessment
{
    BotNeedKind kind = BotNeedKind::None;
    bool urgent = false;
    uint16 score = 0;
    std::string reason;

    std::string KindName() const;
};

// Deterministic arbitration input. This does not issue commands; it decides when
// recovery/survival should own the control slot instead of the LLM.
BotNeedAssessment AssessBotNeeds(Player const* bot);
