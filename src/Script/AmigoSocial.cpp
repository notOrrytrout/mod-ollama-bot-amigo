#include "Script/AmigoSocial.h"

#include "Ai/BotMindState.h"
#include "Ai/LlmDispatch.h"
#include "Ai/OllamaClient.h"
#include "Script/OllamaBotConfig.h"

#include "Group.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "Log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace
{
    using Clock = std::chrono::steady_clock;
    std::mutex gChatterMutex;
    std::unordered_map<uint64, Clock::time_point> gLastChatter;

    std::string Lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    std::string Trim(std::string value)
    {
        auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
        value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
        return value;
    }

    bool IsCommandLike(std::string const& msg)
    {
        std::string trimmed = Trim(msg);
        if (trimmed.empty())
            return true;
        return g_AmigoChatCommandPrefixes.find(trimmed.front()) != std::string::npos;
    }

    bool IsConfiguredBot(Player* player)
    {
        if (!player || !player->IsInWorld())
            return false;
        if (player->GetName() != g_OllamaBotControlBotName)
            return false;
        return PlayerbotsMgr::instance().GetPlayerbotAI(player) != nullptr;
    }

    Player* FindConfiguredBot()
    {
        Player* bot = ObjectAccessor::FindPlayerByName(g_OllamaBotControlBotName);
        return IsConfiguredBot(bot) ? bot : nullptr;
    }

    AmigoSocialIntent ParseIntent(Player* player, std::string const& message)
    {
        AmigoSocialIntent intent;
        intent.sourceGuid = player ? player->GetGUID().GetRawValue() : 0;
        intent.sourceName = player ? player->GetName() : std::string();
        intent.note = message;
        intent.createdAtMs = static_cast<uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

        std::string lower = Lower(Trim(message));
        auto after = [&](std::string const& prefix) -> std::string
        {
            if (lower.rfind(prefix, 0) != 0)
                return {};
            return Trim(message.substr(prefix.size()));
        };

        if (lower == "stop" || lower == "wait" || lower == "stay here")
            intent.kind = AmigoSocialIntentKind::RequestStop;
        else if (lower == "follow me" || lower == "come with me")
            intent.kind = AmigoSocialIntentKind::RequestFollow;
        else if (lower.find("help me") != std::string::npos || lower.find("assist me") != std::string::npos)
            intent.kind = AmigoSocialIntentKind::RequestAssist;
        else if (std::string target = after("gather "); !target.empty())
        {
            intent.kind = AmigoSocialIntentKind::RequestGather;
            intent.target = target;
        }
        else if (std::string target = after("grind "); !target.empty())
        {
            intent.kind = AmigoSocialIntentKind::RequestGrind;
            intent.target = target;
        }
        else if (std::string target = after("kill "); !target.empty())
        {
            intent.kind = AmigoSocialIntentKind::RequestGrind;
            intent.target = target;
        }
        else if (std::string target = after("go to "); !target.empty())
        {
            intent.kind = AmigoSocialIntentKind::RequestTravel;
            intent.target = target;
        }
        else if (lower.find("quest") != std::string::npos && (lower.find("do ") == 0 || lower.find("let's") != std::string::npos))
            intent.kind = AmigoSocialIntentKind::RequestQuest;
        else if (lower.find("let's ") == 0 || lower.find("we should ") == 0)
            intent.kind = AmigoSocialIntentKind::SuggestGoal;

        return intent;
    }

    std::string BuildChatPrompt(uint64 botGuid, std::string const& speaker, std::string const& message)
    {
        std::ostringstream oss;
        oss << "You are " << g_OllamaBotControlBotName << ", a World of Warcraft player character controlled by Amigo.\n";
        oss << "Speak like a player, not like an assistant. Keep the reply short and natural.\n";
        oss << "Do not claim you performed an action unless CURRENT MIND STATE says it happened.\n";
        oss << "Do not emit commands, JSON, tool names, or implementation details.\n";
        if (g_AmigoGroupAuthority == "peer")
            oss << "Party members, including the party leader, are peers rather than owners. Explicit follow/assist requests are temporary cooperation, not permanent authority.\n";
        if (g_AmigoPersonalityEnable)
        {
            oss << "Personality: " << g_AmigoPersonalityName << ".\n";
            if (!g_AmigoPersonalityPrompt.empty())
                oss << g_AmigoPersonalityPrompt << "\n";
        }
        oss << "\nCURRENT MIND STATE\n" << AmigoMindBuildPromptContext(botGuid, g_AmigoChatHistorySize) << "\n";
        oss << "PLAYER MESSAGE\n" << speaker << ": " << message << "\n\n";
        oss << "Reply with one short in-character line.";
        return oss.str();
    }

    void QueueReply(Player* bot, Player* speaker, std::string message, bool whisper)
    {
        if (!bot || !speaker || message.empty())
            return;

        uint64 botGuid = bot->GetGUID().GetRawValue();
        uint64 speakerGuid = speaker->GetGUID().GetRawValue();
        std::string speakerName = speaker->GetName();
        std::string prompt = BuildChatPrompt(botGuid, speakerName, message);
        std::string model = g_OllamaBotControlChatModel;

        AmigoSocialIntent intent = ParseIntent(speaker, message);
        if (intent.kind != AmigoSocialIntentKind::None)
            AmigoMindPromoteIntent(botGuid, std::move(intent));

        bool queued = AmigoLlmDispatchSubmit([botGuid, speakerGuid, speakerName, message = std::move(message), prompt = std::move(prompt), model = std::move(model), whisper]() mutable
        {
            AmigoOllamaResult result = QueryOllamaLLMEx(model, prompt, false);
            if (!result.ok || result.text.empty())
                return;

            std::string reply = Trim(result.text);
            if (reply.size() > 320)
                reply.resize(320);

            AmigoLlmDispatchPostCompletion([botGuid, speakerGuid, speakerName, message = std::move(message), reply = std::move(reply), whisper]() mutable
            {
                Player* liveBot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(botGuid));
                Player* liveSpeaker = ObjectAccessor::FindConnectedPlayer(ObjectGuid(speakerGuid));
                if (!IsConfiguredBot(liveBot) || !liveSpeaker)
                    return;

                PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(liveBot);
                if (!ai)
                    return;

                if (whisper)
                    ai->Whisper(reply, liveSpeaker->GetName());
                else if (liveBot->GetGroup() && liveSpeaker->GetGroup() == liveBot->GetGroup())
                    ai->SayToParty(reply);
                else
                    return;

                AmigoSocialTurn turn;
                turn.speakerGuid = speakerGuid;
                turn.speakerName = speakerName;
                turn.playerText = message;
                turn.botText = reply;
                turn.atMs = static_cast<uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
                AmigoMindRecordSocialTurn(botGuid, std::move(turn), g_AmigoChatHistorySize);
            });
        });

        if (!queued && g_EnableOllamaBotAmigoDebug)
            LOG_INFO("server.loading", "[OllamaBotAmigo] Social reply dropped because LLM queue is full.");
    }

    bool EventCooldownReady(uint64 botGuid)
    {
        std::lock_guard<std::mutex> lock(gChatterMutex);
        auto now = Clock::now();
        auto it = gLastChatter.find(botGuid);
        if (it != gLastChatter.end() && g_AmigoEventChatterCooldownMs > 0)
        {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count();
            if (elapsed < g_AmigoEventChatterCooldownMs)
                return false;
        }
        gLastChatter[botGuid] = now;
        return true;
    }

    void QueueEventChatter(Player* bot, std::string eventText)
    {
        if (!g_AmigoChatEnable || !g_AmigoEventChatterEnable || !IsConfiguredBot(bot) || !EventCooldownReady(bot->GetGUID().GetRawValue()))
            return;
        Group* group = bot->GetGroup();
        if (g_AmigoChatPartyOnly && !group)
            return;

        uint64 botGuid = bot->GetGUID().GetRawValue();
        std::ostringstream prompt;
        prompt << "You are " << bot->GetName() << ", a World of Warcraft player. Say one brief in-character party line reacting to this event: " << eventText << ".\n";
        if (g_AmigoPersonalityEnable && !g_AmigoPersonalityPrompt.empty())
            prompt << "Personality: " << g_AmigoPersonalityPrompt << "\n";
        prompt << "Current mind state:\n" << AmigoMindBuildPromptContext(botGuid, 2);

        std::string model = g_OllamaBotControlChatModel;
        AmigoLlmDispatchSubmit([botGuid, promptText = prompt.str(), model = std::move(model)]() mutable
        {
            AmigoOllamaResult result = QueryOllamaLLMEx(model, promptText, false);
            if (!result.ok || result.text.empty())
                return;
            std::string reply = Trim(result.text);
            if (reply.size() > 240) reply.resize(240);
            AmigoLlmDispatchPostCompletion([botGuid, reply = std::move(reply)]()
            {
                Player* liveBot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(botGuid));
                if (!IsConfiguredBot(liveBot) || !liveBot->GetGroup())
                    return;
                if (PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(liveBot))
                    ai->SayToParty(reply);
            });
        });
    }
}

AmigoSocialScript::AmigoSocialScript()
    : PlayerScript("AmigoSocialScript", { PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT, PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT })
{
}

bool AmigoSocialScript::OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 /*lang*/, std::string& msg, Player* receiver)
{
    if (!g_AmigoChatEnable || g_AmigoChatPartyOnly || !player || !receiver || IsCommandLike(msg))
        return true;
    if (!IsConfiguredBot(receiver))
        return true;
    QueueReply(receiver, player, msg, true);
    return true;
}

bool AmigoSocialScript::OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 /*lang*/, std::string& msg, Group* group)
{
    if (!g_AmigoChatEnable || !player || !group || IsCommandLike(msg))
        return true;

    Player* bot = FindConfiguredBot();
    if (!bot || bot == player || bot->GetGroup() != group)
        return true;

    QueueReply(bot, player, msg, false);
    return true;
}

AmigoEventChatterScript::AmigoEventChatterScript()
    : PlayerScript("AmigoEventChatterScript", { PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST, PLAYERHOOK_ON_PLAYER_JUST_DIED })
{
}

void AmigoEventChatterScript::OnPlayerLevelChanged(Player* player, uint8 oldLevel)
{
    if (player && player->GetLevel() > oldLevel)
        QueueEventChatter(player, "I reached level " + std::to_string(player->GetLevel()));
}

void AmigoEventChatterScript::OnPlayerCompleteQuest(Player* player, Quest const* quest)
{
    if (player && quest)
        QueueEventChatter(player, "I completed the quest " + quest->GetTitle());
}

void AmigoEventChatterScript::OnPlayerJustDied(Player* player)
{
    QueueEventChatter(player, "I just died");
}
