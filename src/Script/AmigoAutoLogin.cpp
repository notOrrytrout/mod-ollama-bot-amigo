#include "Script/AmigoAutoLogin.h"

#include "Script/OllamaBotConfig.h"
#include "Ai/OllamaRuntime.h"
#include "Util/AmigoBotNames.h"

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

    std::vector<std::string> names = ParseAmigoBotNames(g_OllamaBotControlBotName);
    bool missingCharacter = false;
    for (std::string const& botName : names)
    {
        ObjectGuid botGuid = sCharacterCache->GetCharacterGuidByName(botName);
        if (botGuid.IsEmpty())
        {
            missingCharacter = true;
            if (!loggedMissingCharacter_)
            {
                LOG_ERROR("server.loading", "[OllamaBotAmigo] Auto-login could not find configured character '{}'.", botName);
                loggedMissingCharacter_ = true;
            }
            continue;
        }

        // Never interfere with a character that is already connected. This also
        // covers a real client logged into the configured character.
        if (Player* online = ObjectAccessor::FindConnectedPlayer(botGuid))
        {
            if (online->IsInWorld())
                continue;
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
                     botName,
                     botGuid.GetRawValue());
        }
    }

    if (!missingCharacter)
        loggedMissingCharacter_ = false;
}
