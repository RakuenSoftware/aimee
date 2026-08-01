import importlib.util
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
VALIDATOR_PATH = ROOT / "benchmarks" / "code-agent-effectiveness" / "validate_fixtures.py"


class AgentCodeIntelligenceFixtureTests(unittest.TestCase):
    def test_fixture_contract(self):
        spec = importlib.util.spec_from_file_location("agent_code_fixture_validator", VALIDATOR_PATH)
        self.assertIsNotNone(spec)
        self.assertIsNotNone(spec.loader)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.validate(verify_sources=True)


if __name__ == "__main__":
    unittest.main()
