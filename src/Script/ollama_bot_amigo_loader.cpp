#include "Script/OllamaBotConfig.h"
#include "Script/AmigoControlControllerScript.h"
#include "Script/AmigoPlanner.h"
#include "Script/OllamaBotControlLoop.h"
#include "Log.h"

void Addmod_ollama_bot_amigoScripts()
{
    // Register config loader first so global settings are ready for other scripts.
    new OllamaBotControlConfigWorldScript();
    LOG_INFO("server.loading", "Registering mod-ollama-bot-amigo scripts.");
    // Register the control loop, planner applier, and per-player control handlers.
    new OllamaBotControlLoop();
    new AmigoPlannerApplierScript();
    new AmigoControlControllerScript();
    new AmigoBotLoginScript();
}
