#pragma once

#include "ScriptMgr.h"
#include "Define.h"

class AmigoAutoLoginScript : public WorldScript
{
public:
    AmigoAutoLoginScript();

    void OnStartup() override;
    void OnUpdate(uint32 diff) override;

private:
    uint32 retryRemainingMs_ = 0;
    bool loggedMissingCharacter_ = false;
};
