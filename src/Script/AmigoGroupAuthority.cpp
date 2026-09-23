#include "Script/AmigoGroupAuthority.h"

#include "Ai/BotMindState.h"
#include "Ai/OllamaRuntime.h"
#include "Script/OllamaBotConfig.h"

#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Timer.h"
#include "WorldSession.h"

#include <utility>

namespace
{
    constexpr uint32 kAuthorityUpdateMs = 250;

    Player* FindConfiguredBot()
    {
        if (g_OllamaBotControlBotName.empty())
            return nullptr;
        Player* bot = ObjectAccessor::FindPlayerByName(g_OllamaBotControlBotName);
        if (!bot || !bot->IsInWorld())
            return nullptr;
        return PlayerbotsMgr::instance().GetPlayerbotAI(bot) ? bot : nullptr;
    }

    bool SameGroup(Player const* a, Player const* b)
    {
        return a && b && a->GetGroup() && a->GetGroup() == b->GetGroup();
    }

    void NormalizePeer(Player* bot, PlayerbotAI* ai)
    {
        if (!bot || !ai || !bot->GetGroup())
            return;

        ai->SetMaster(nullptr);

        if (ai->HasStrategy("follow", BOT_STATE_NON_COMBAT))
            ai->ChangeStrategy("-follow", BOT_STATE_NON_COMBAT);
    }
}

AmigoGroupAuthorityScript::AmigoGroupAuthorityScript()
    : WorldScript("AmigoGroupAuthorityScript")
{
}

void AmigoGroupAuthorityScript::ClearDirective(bool normalizePeer, char const* reason)
{
    (void)normalizePeer;
    Player* bot = activeBotGuid_ ? ObjectAccessor::FindConnectedPlayer(ObjectGuid(activeBotGuid_)) : FindConfiguredBot();
    if (bot)
    {
        if (PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
        {
            if (ai->HasStrategy("follow", BOT_STATE_NON_COMBAT))
                ai->ChangeStrategy("-follow", BOT_STATE_NON_COMBAT);

            if (bot->GetSession() && !bot->GetSession()->IsBot())
                ai->SetMaster(bot);
            else
                ai->SetMaster(nullptr);
        }
        AmigoMindClearPeerDirective(bot->GetGUID().GetRawValue());
    }
    else if (activeBotGuid_)
    {
        AmigoMindClearPeerDirective(activeBotGuid_);
    }

    if (g_EnableOllamaBotAmigoDebug && activeTargetGuid_)
    {
        LOG_INFO("server.loading",
                 "[OllamaBotAmigo] Temporary peer directive cleared: bot_guid={} target_guid={} reason={}",
                 activeBotGuid_, activeTargetGuid_, reason ? reason : "unknown");
    }

    activeBotGuid_ = 0;
    activeTargetGuid_ = 0;
    activeUntilMs_ = 0;
    activeKind_ = 0;
}

void AmigoGroupAuthorityScript::OnUpdate(uint32 diff)
{
    if (updateRemainingMs_ > diff)
    {
        updateRemainingMs_ -= diff;
        return;
    }
    updateRemainingMs_ = kAuthorityUpdateMs;

    Player* bot = FindConfiguredBot();
    if (!bot)
    {
        if (activeBotGuid_)
            ClearDirective(false, "bot_offline");
        return;
    }

    PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    if (!ai)
        return;

    const uint64 botGuid = bot->GetGUID().GetRawValue();
    // Playerbots uses self-master identity to send object updates to a real
    // client. Peer ownership changes must never remove that identity.
    if (bot->GetSession() && !bot->GetSession()->IsBot())
    {
        if (activeBotGuid_)
            ClearDirective(false, "self_bot_client");
        if (ai->GetMaster() != bot)
            ai->SetMaster(bot);
        return;
    }

    if (!g_OllamaBotRuntime.enable_control)
    {
        if (activeBotGuid_)
            ClearDirective(false, "amigo_disabled");
        else if (ai->GetMaster() == bot)
            ai->SetMaster(nullptr);
        return;
    }

    // Legacy escape hatch for users who explicitly want Playerbots' native
    // master/leader ownership behavior. Clear peer containment so Playerbots
    // can elect its usual master on the next AI update.
    if (g_AmigoGroupAuthority == "playerbots")
    {
        if (activeBotGuid_)
            ClearDirective(false, "playerbots_authority_enabled");
        else
        {
            AmigoMindClearPeerDirective(botGuid);
            if (ai->GetMaster() == bot)
                ai->SetMaster(nullptr);
        }
        return;
    }

    // Outside a group there is no peer/master relationship to contain.
    if (!bot->GetGroup())
    {
        if (activeBotGuid_)
            ClearDirective(true, "left_group");
        ai->SetMaster(nullptr);
        AmigoMindClearPeerDirective(botGuid);
        return;
    }

    const uint32 nowMs = getMSTime();

    // Expire or invalidate an active explicit peer request.
    if (activeBotGuid_ == botGuid && activeTargetGuid_)
    {
        Player* target = ObjectAccessor::FindConnectedPlayer(ObjectGuid(activeTargetGuid_));
        if (!target || !SameGroup(bot, target))
        {
            ClearDirective(true, "target_left_group");
        }
        else if (activeUntilMs_ && nowMs >= activeUntilMs_)
        {
            ClearDirective(true, "expired");
        }
        else
        {
            // Preserve the explicitly selected temporary target. Playerbots
            // may use this pointer internally for formation/assist mechanics,
            // but Amigo remains the authority and the relation is time-bounded.
            if (ai->GetMaster() != target)
                ai->SetMaster(target);

            if (activeKind_ == static_cast<uint8>(AmigoPeerDirectiveKind::Follow))
            {
                if (!ai->HasStrategy("follow", BOT_STATE_NON_COMBAT))
                    ai->ChangeStrategy("+follow", BOT_STATE_NON_COMBAT);
            }
            else if (ai->HasStrategy("follow", BOT_STATE_NON_COMBAT))
            {
                ai->ChangeStrategy("-follow", BOT_STATE_NON_COMBAT);
            }
        }
    }

    AmigoBotMindSnapshot mind = AmigoMindSnapshot(botGuid);
    AmigoSocialIntent const& intent = mind.pendingIntent;
    if (intent.id && !intent.acknowledged)
    {
        if (intent.kind == AmigoSocialIntentKind::RequestFollow || intent.kind == AmigoSocialIntentKind::RequestAssist)
        {
            Player* requester = intent.sourceGuid ? ObjectAccessor::FindConnectedPlayer(ObjectGuid(intent.sourceGuid)) : nullptr;
            if (requester && requester != bot && SameGroup(bot, requester))
            {
                if (activeBotGuid_ && activeTargetGuid_)
                    ClearDirective(true, "replaced");

                activeBotGuid_ = botGuid;
                activeTargetGuid_ = requester->GetGUID().GetRawValue();
                activeUntilMs_ = nowMs + g_AmigoGroupDirectiveTtlMs;
                activeKind_ = static_cast<uint8>(intent.kind == AmigoSocialIntentKind::RequestFollow
                    ? AmigoPeerDirectiveKind::Follow
                    : AmigoPeerDirectiveKind::Assist);

                ai->SetMaster(requester);
                if (intent.kind == AmigoSocialIntentKind::RequestFollow)
                    ai->ChangeStrategy("+follow", BOT_STATE_NON_COMBAT);
                else if (ai->HasStrategy("follow", BOT_STATE_NON_COMBAT))
                    ai->ChangeStrategy("-follow", BOT_STATE_NON_COMBAT);

                AmigoPeerDirective directive;
                directive.kind = intent.kind == AmigoSocialIntentKind::RequestFollow
                    ? AmigoPeerDirectiveKind::Follow
                    : AmigoPeerDirectiveKind::Assist;
                directive.targetGuid = activeTargetGuid_;
                directive.targetName = requester->GetName();
                AmigoMindSetPeerDirective(botGuid, std::move(directive));
                AmigoMindAcknowledgeIntent(botGuid, intent.id);

                if (g_EnableOllamaBotAmigoDebug)
                {
                    LOG_INFO("server.loading",
                             "[OllamaBotAmigo] Temporary peer directive accepted for {}: kind={} target={} ttl_ms={}",
                             bot->GetName(),
                             intent.kind == AmigoSocialIntentKind::RequestFollow ? "follow" : "assist",
                             requester->GetName(),
                             g_AmigoGroupDirectiveTtlMs);
                }
            }
            else
            {
                // Reject follow/assist requests from outside the current party.
                AmigoMindAcknowledgeIntent(botGuid, intent.id);
                if (g_EnableOllamaBotAmigoDebug)
                    LOG_INFO("server.loading", "[OllamaBotAmigo] Peer directive rejected: requester is not in {}'s group.", bot->GetName());
            }
        }
        else if (intent.kind == AmigoSocialIntentKind::RequestStop && activeBotGuid_ == botGuid && activeTargetGuid_)
        {
            AmigoMindAcknowledgeIntent(botGuid, intent.id);
            ClearDirective(true, "explicit_stop");
        }
    }

    // If no explicit directive owns the relationship, continuously enforce
    // peer semantics so Playerbots cannot silently promote the group leader.
    if (!(activeBotGuid_ == botGuid && activeTargetGuid_))
        NormalizePeer(bot, ai);
}
