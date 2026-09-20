"""Source integration checks for the Playerbots/Amigo boundary."""
from pathlib import Path
import unittest

AMIGO = Path(__file__).resolve().parents[1]
PLAYERBOTS = AMIGO.parent / "mod-playerbots"


class IntegrationBoundaryTests(unittest.TestCase):
    def test_external_authority_blocks_both_master_election_paths(self):
        source = (PLAYERBOTS / "src/Bot/PlayerbotAI.cpp").read_text()
        update = source.split("void PlayerbotAI::UpdateAIGroupMaster()", 1)[1]
        update = update.split("void PlayerbotAI::UpdateAIInternal", 1)[0]
        self.assertLess(update.index("externalAuthority"), update.index("FindNewMaster()"))
        election = source.split("Player* PlayerbotAI::FindNewMaster()", 1)[1]
        self.assertLess(election.index("if (externalAuthority)"), election.index("GetGroup()"))
        authority = (AMIGO / "src/Script/AmigoGroupAuthority.cpp").read_text()
        normalize = authority.split("void NormalizePeer", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("SetExternalAuthority(true)", normalize)
        self.assertIn("SetMaster(nullptr)", normalize)
        self.assertNotIn("SetMaster(bot)", normalize)
        invitation = (PLAYERBOTS / "src/Ai/Base/Actions/AcceptInvitationAction.cpp").read_text()
        self.assertIn("!botAI->HasExternalAuthority() && sRandomPlayerbotMgr.IsRandomBot(bot)", invitation)

    def test_only_bobber_use_records_success(self):
        fishing = (PLAYERBOTS / "src/Ai/Base/Actions/FishingAction.cpp").read_text()
        use = fishing.split("bool UseBobberAction::Execute", 1)[1].split("bool EndMasterFishingAction", 1)[0]
        self.assertLess(use.index("go->Use(bot)"), use.index("RecordFishingCompletion()"))
        remove = fishing.split("bool RemoveBobberStrategyAction::Execute", 1)[1]
        self.assertNotIn("RecordFishingCompletion", remove)
        observer = (AMIGO / "src/Bot/BotProfession.cpp").read_text()
        update = observer.split("void BotProfession::Update", 1)[1].split("void BotProfession::Abort", 1)[0]
        self.assertEqual(update.count("ProfessionResult::Succeeded"), 1)
        self.assertLess(update.index("GetFishingCompletions() != fishingCompletionBaseline_"),
                        update.index("ProfessionResult::Succeeded"))
        removed = update.split("if (bobberStrategySeen_)", 1)[1]
        self.assertIn("ProfessionResult::Aborted", removed)
        self.assertNotIn("ProfessionResult::Succeeded", removed)


if __name__ == "__main__":
    unittest.main()
