#include "Script/AmigoAutoLogin.h"

#include "Script/OllamaBotConfig.h"
#include "Ai/OllamaRuntime.h"

#include "CharacterCache.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "RandomPlayerbotMgr.h"

AmigoAutoLoginScript::AmigoAutoLoginScript()
    : WorldScript("AmigoAutoLoginScript")
{
}

void AmigoAutoLoginScript::OnStartup()
{
    retryRemainingMs_ = g_AmigoBotAutoLoginDelayMs;
}

void AmigoAutoLoginScript::OnUpdate(uint32 diff)
{
    if (!g_OllamaBotRuntime.enable_control || !g_AmigoBotAutoLogin || g_OllamaBotControlBotName.empty())
        return;

    if (retryRemainingMs_ > diff)
    {
        retryRemainingMs_ -= diff;
        return;
    }
    retryRemainingMs_ = g_AmigoBotAutoLoginRetryMs;

    ObjectGuid botGuid = sCharacterCache->GetCharacterGuidByName(g_OllamaBotControlBotName);
    if (botGuid.IsEmpty())
    {
        if (!loggedMissingCharacter_)
        {
            LOG_ERROR("server.loading",
                      "[OllamaBotAmigo] Auto-login could not find configured character '{}'.",
                      g_OllamaBotControlBotName);
            loggedMissingCharacter_ = true;
        }
        return;
    }

    loggedMissingCharacter_ = false;

    // Never interfere with a character that is already connected. This also
    // covers a real client logged into the configured character.
    if (Player* online = ObjectAccessor::FindConnectedPlayer(botGuid))
    {
        if (online->IsInWorld())
            return;
    }

    // IMPORTANT: masterAccountId must be zero here. A non-zero master account
    // makes Playerbots apply its "may this player control that bot?" ownership
    // rules. Amigo's configured bot is autonomous, so use the random-manager
    // holder as the owner while still going through Playerbots' normal
    // asynchronous character/session login path.
    sRandomPlayerbotMgr.AddPlayerBot(botGuid, 0);

    if (g_EnableOllamaBotAmigoDebug)
    {
        LOG_INFO("server.loading",
                 "[OllamaBotAmigo] Requested autonomous Playerbots login for '{}' (guid={}, masterAccountId=0).",
                 g_OllamaBotControlBotName,
                 botGuid.GetRawValue());
    }
}
