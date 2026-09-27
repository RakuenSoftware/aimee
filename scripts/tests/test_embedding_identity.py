"""Pin the Go/provider identity commitment without loading a model."""
import ast
import copy
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class EmbeddingIdentityTest(unittest.TestCase):
    def test_cross_language_commitment_and_same_dimension_changes(self):
        source = ROOT / 'scripts/embedder-server.py'
        tree = ast.parse(source.read_text())
        node = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'embedding_identity_digest')
        namespace = {}
        exec(compile(ast.Module(body=[node], type_ignores=[]), str(source), 'exec'), namespace)
        digest = namespace['embedding_identity_digest']
        fixture = json.loads((ROOT / 'tests/eval/memory_mr11/embedding-identity.json').read_text())
        self.assertEqual(digest(fixture['identity']), fixture['digest'])
        for field in ('artifact_revision', 'tokenizer_revision', 'query_preprocessing', 'document_preprocessing', 'query_prefix', 'document_prefix', 'pooling', 'normalization'):
            changed = copy.deepcopy(fixture['identity'])
            changed[field] += 'changed'
            self.assertNotEqual(digest(changed), fixture['digest'], field)
        changed = copy.deepcopy(fixture['identity'])
        changed['provider_configuration']['quantization'] = 'int8'
        self.assertNotEqual(digest(changed), fixture['digest'])


if __name__ == '__main__':
    unittest.main()
