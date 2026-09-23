"""Source integration checks for Amigo's Playerbots boundaries."""
from pathlib import Path
import unittest

AMIGO = Path(__file__).resolve().parents[1]
PLAYERBOTS = AMIGO.parent / "mod-playerbots"


class IntegrationBoundaryTests(unittest.TestCase):
    def test_logout_retires_old_session_before_later_results_can_apply(self):
        login = (AMIGO / "src/Script/AmigoPlanner.cpp").read_text()
        loop = (AMIGO / "src/Script/OllamaBotControlLoop.cpp").read_text()
        cleanup = loop.split("void RetireAmigoBotState(Player* player)", 1)[1]
        cleanup = cleanup.split("static uint32 ConsumeLongTermPlannerRefresh", 1)[0]
        self.assertIn("RetireAmigoBotState(player)", login)
        for required in ("state->retired = true", "state->movement.Abort", "state->memory.FlushPending()",
                         "ControlActionRegistry::Instance().Clear(guid)",
                         "AmigoPlannerRegistry::Instance().Clear(guid)",
                         "ClearAmigoPendingControl(guid)", "botStates.erase(it)"):
            self.assertIn(required, cleanup)
        self.assertEqual(loop.count("!stateRef->retired && BotMissionRegistry::Instance().RevisionMatches"), 2)

    def test_quest_object_execution_rechecks_live_quest_and_object(self):
        catalog = (AMIGO / "src/Ai/ControlAction.cpp").read_text()
        controller = (AMIGO / "src/Script/AmigoControlControllerScript.cpp").read_text()
        branch = controller.split("if (actionState.action.capability == ControlAction::Capability::UseQuestObject)", 1)[1]
        branch = branch.split("if (actionState.action.capability == ControlAction::Capability::GatherTarget)", 1)[0]
        self.assertIn("request_use_quest_object", catalog)
        for required in ("QUEST_STATUS_INCOMPLETE", "RequiredNpcOrGo[index]", "CreatureOrGOCount[index]",
                         "INTERACTION_DISTANCE", "IsWithinLOSInMap", "target->Use(player)"):
            self.assertIn(required, branch)

    def test_fishing_source_loot_records_success(self):
        controller = (AMIGO / "src/Script/AmigoControlControllerScript.cpp").read_text()
        hook = controller.split("void AmigoControlControllerScript::OnPlayerLootItem", 1)[1]
        hook = hook.split("AmigoControlControllerScript::AmigoControlControllerScript", 1)[0]
        self.assertIn("GAMEOBJECT_TYPE_FISHINGNODE", hook)
        self.assertIn("GAMEOBJECT_TYPE_FISHINGHOLE", hook)
        self.assertIn("fishingSource->GetOwnerGUID() == player->GetGUID()", hook)
        self.assertIn("profession->RecordFishingCatch", hook)
        profession = (AMIGO / "src/Bot/BotProfession.cpp").read_text()
        completion = profession.split("bool BotProfession::RecordFishingCatch", 1)[1]
        self.assertIn("if (!active_ || activity_ != ProfessionActivity::Fishing)", completion)
        self.assertIn("lastResult_ = ProfessionResult::Succeeded", completion)


if __name__ == "__main__":
    unittest.main()
