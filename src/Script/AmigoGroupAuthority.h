#pragma once

#include "ScriptMgr.h"
#include "Define.h"

class AmigoGroupAuthorityScript : public WorldScript
{
public:
    AmigoGroupAuthorityScript();
    void OnUpdate(uint32 diff) override;

private:
    uint32 updateRemainingMs_ = 0;
    uint64 activeBotGuid_ = 0;
    uint64 activeTargetGuid_ = 0;
    uint32 activeUntilMs_ = 0;
    uint8 activeKind_ = 0;

    void ClearDirective(bool normalizePeer, char const* reason);
};
