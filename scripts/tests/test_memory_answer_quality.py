"""Do not score an explicit source correction as a contradictory assertion."""
import importlib.util
from pathlib import Path
import unittest
spec = importlib.util.spec_from_file_location('answer_quality', Path(__file__).resolve().parents[1] / 'validation/memory/answer-quality.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class AnswerQualityTests(unittest.TestCase):
    def test_preserved_negation_and_contradiction(self):
        self.assertFalse(module.affirmative_mentions('Virant said 7kg, not 7 pounds.', '7 pounds'))
        self.assertFalse(module.affirmative_mentions('The person is 69cm rather than 69 feet.', '69 feet'))
        self.assertTrue(module.affirmative_mentions('The pack weighs 7 pounds.', '7 pounds'))
        self.assertTrue(module.affirmative_mentions('Not 7 pounds, but another report says 7 pounds.', '7 pounds'))

if __name__ == '__main__':
    unittest.main()
