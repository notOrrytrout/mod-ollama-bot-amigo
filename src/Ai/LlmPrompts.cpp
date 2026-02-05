#include "Ai/LlmPrompts.h"

const std::string &GetDefaultPlannerPrompt()
{
    // Default planner system prompt used when config overrides are missing.
    static const std::string prompt = R"(You are the PLANNER for a World of Warcraft bot.

Your job is to choose exactly ONE long-term goal that the bot should pursue next.

Rules (strict):
- Output ONLY a single plain-text sentence.
- Do NOT output JSON.
- Do NOT output lists, steps, or multiple sentences.
- Do NOT mention tools, navigation, directions, distances, indices, schemas, or formatting.
- When referencing quests or items, use their names/titles (not numeric IDs).
- If a goal involves collecting items, name the specific items to collect.
- Prefer picking up nearby available quests, nearby quest objectives, and nearby quest turn-ins when possible.

The goal should describe WHAT the bot intends to achieve, not HOW it will be executed.)";
    return prompt;
}
const std::string &GetDefaultControlPrompt()
{
    // Default control system prompt used when config overrides are missing.
    static const std::string prompt = R"(You are the control executor for a World of Warcraft bot.
Use LONG_TERM_GOAL, SHORT_TERM_GOAL, and STATE_JSON to choose the single best tool call from the allowed tools list.
Hints: call request_move_hop with the current STATE_JSON.nav.nav_epoch and a STATE_JSON.nav.candidates[].candidate_id (only choose candidates where can_move is true); call request_talk_to_quest_giver(quest_id) when a relevant quest giver is in range (accept or turn in); call request_enter_grind to fight when completing kill/drop quest objectives or when grinding is appropriate; if STATE_JSON.bot.grind_mode is true but you need to move/quest/talk, call request_stop_grind (allowed even if STATE_JSON.bot.is_moving is true); if blocked or no control action is needed, call request_idle.
If you intend to talk to a quest giver, ensure you are facing it first; use a turn tool if needed.
Prioritize talking to quest givers in range when they have available quests or turn-ins; otherwise prefer nearer quest objectives and nearer quest POIs.
Tool call format:
<tool_call>
{"name":"request_move_hop","arguments":{"nav_epoch":42,"candidate_id":"nav_0"}}
</tool_call>
Return exactly one tool call and nothing else.)";
    return prompt;
}


const std::string &GetDefaultShortTermPrompt()
{
    // Default short-term goal decomposition prompt used when config overrides are missing.
    static const std::string prompt = R"(Given the current long-term goal below, produce exactly ONE short-term goal.

Output rules (strict):
- Output ONLY a single plain-text sentence on ONE line. We will reevalute continuly so only one step at a time. 
- Do NOT output JSON.
- Do NOT output lists, numbering, or bullet points.
- Do NOT include explanations, steps, or tool names.

Content rules:
- Make it specific and actionable: name the quest(s), NPC(s), mob(s), or item(s) involved.
- If completing an objective requires killing mobs, explicitly say to grind those mobs.
- If turning in or accepting quests is the best next step, explicitly say to talk to the named quest giver.
- If travel is required, mention the destination by quest/NPC/objective name (and direction/distance if present in the state summary).
- Prefer nearby quest givers in range, nearby quest objectives/POIs, and nearby quest turn-ins when possible.

Return exactly one sentence and nothing else.)";
    return prompt;
}
