#pragma once

#include <cstdint>

// Professions/external activities executed by the controller.
// Execution is stateful, tick-driven, and interruptible.

enum class ProfessionActivity : uint8_t
{
    None = 0,
    Fishing = 1,
};

enum class ProfessionResult : uint8_t
{
    None = 0,
    Started = 1,
    Succeeded = 2,
    FailedTemporary = 3,
    FailedPermanent = 4,
    Aborted = 5,
    TimedOut = 6,
};
