#include "mod-ollama-bot-buddy_handler.h"
#include "Log.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "ObjectAccessor.h"
#include <mutex>
#include <algorithm>
#include <unordered_map>
#include <deque>
#include <chrono>

// Uchovává poslední zprávy: [bot GUID][playerName] => pair<text, timestamp>
std::unordered_map<uint64_t, std::unordered_map<std::string, std::pair<std::string, std::chrono::steady_clock::time_point>>> lastMessages;
std::mutex botPlayerMessagesMutex;

void BotBuddyChatHandler::OnPlayerChat(Player* player, uint32_t type, uint32_t lang, std::string& msg)
{
    ProcessChat(player, type, lang, msg, nullptr);
}
void BotBuddyChatHandler::OnPlayerChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Group* /*group*/)
{
    ProcessChat(player, type, lang, msg, nullptr);
}
void BotBuddyChatHandler::OnPlayerChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Channel* channel)
{
    ProcessChat(player, type, lang, msg, channel);
}

void BotBuddyChatHandler::ProcessChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Channel* channel)
{
    LOG_INFO("server.loading", "ProcessChat: sender={} type={} lang={} msg='{}' channel={}", 
    player ? player->GetName() : "NULL", type, lang, msg, channel ? channel->GetName() : "nullptr");

    if (!player || msg.empty()) return;
    PlayerbotAI* senderAI = sPlayerbotsMgr->GetPlayerbotAI(player);
    if (senderAI && senderAI->IsBotAI()) return;

    std::lock_guard<std::mutex> lock(botPlayerMessagesMutex);

    auto const& allPlayers = ObjectAccessor::GetPlayers();
    for (auto const& itr : allPlayers)
    {
        Player* bot = itr.second;
        if (!bot || !bot->IsAlive()) continue;

        PlayerbotAI* botAI = sPlayerbotsMgr->GetPlayerbotAI(bot);
        if (!botAI || !botAI->IsBotAI()) continue;

        std::string messageLower = msg;
        std::string botNameLower = bot->GetName();
        std::transform(messageLower.begin(), messageLower.end(), messageLower.begin(), ::tolower);
        std::transform(botNameLower.begin(), botNameLower.end(), botNameLower.begin(), ::tolower);

        // Pokud hráč zmiňuje bota ve zprávě
        if (messageLower.find(botNameLower) != std::string::npos)
        {
            uint64_t botGuid = bot->GetGUID().GetRawValue();
            std::string playerName = player->GetName();
            auto& lastMsgMap = lastMessages[botGuid];

            auto now = std::chrono::steady_clock::now();
            constexpr auto cooldown = std::chrono::seconds(3); // min. interval mezi stejnou zprávou

            // Kontrola na spam: pokud poslal stejnou zprávu před chvílí, ignorujeme
            auto it = lastMsgMap.find(playerName);
            if (it != lastMsgMap.end()) {
                const auto& [lastText, lastTime] = it->second;
                if (lastText == msg && (now - lastTime) < cooldown) {
                    LOG_DEBUG("server.loading", "Duplicate chat command from player={} to bot={}, ignored.", playerName, botNameLower);
                    continue; // Přeskočit tuto zprávu
                }
            }
            // Uložit poslední zprávu a čas
            lastMsgMap[playerName] = std::make_pair(msg, now);

            // Tady bys mohl mít původní ukládání zprávy, pokud není spam:
            // botPlayerMessages[botGuid].emplace_back(playerName, msg);
            // Pokud chceš zpětně kompatibilní buffer, můžeš zachovat tuto queue:
            // případně limituj velikost fronty (třeba posledních 20 zpráv)
        }
    }
}

