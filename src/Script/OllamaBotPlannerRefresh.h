#pragma once

#include "Define.h"

// Request an out-of-band refresh of the long-term planner for a given bot GUID.
// The request is consumed by the main-thread planner scheduler in OllamaBotControlLoop.
void RequestLongTermPlannerRefresh(uint64 guid, uint32 nowMs);

