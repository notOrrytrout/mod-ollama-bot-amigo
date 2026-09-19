#pragma once

// Playerbots owns WorldPosition in TravelMgr.h. Keep this compatibility
// header intentionally small so callers do not inherit unrelated headers or
// create circular include dependencies.
#include "TravelMgr.h"
