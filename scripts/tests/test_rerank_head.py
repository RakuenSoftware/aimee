#!/usr/bin/env python3
"""Unit tests for scripts/aimee_llm_rerank_head.py (the ettin gateway reranker head).

Pure numpy — no model, no torch, no network. Verifies the head MATH against a hand
reference, the rerank ordering, the from_dir safetensors loader, and edge cases.
Skipped if numpy/safetensors are unavailable (the head runs inside the aimee-llm
container, which has them; CI installs them for this test).
"""
import importlib.util
import os
import tempfile
import unittest

try:
    import numpy as np

    HAVE_NUMPY = True
except ImportError:  # pragma: no cover
    HAVE_NUMPY = False

SCRIPT = os.path.join(os.path.dirname(__file__), "..", "aimee_llm_rerank_head.py")


def _load_module():
    spec = importlib.util.spec_from_file_location("rerank_head", SCRIPT)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


@unittest.skipUnless(HAVE_NUMPY, "numpy required")
class HeadMath(unittest.TestCase):
    def setUp(self):
        self.m = _load_module()

    def test_gelu_layernorm_reference(self):
        """Head reproduces an independent numpy reference for known weights."""
        rng = np.random.default_rng(0)
        D = 8
        W2 = rng.standard_normal((D, D)).astype(np.float32)
        W4 = rng.standard_normal((1, D)).astype(np.float32)
        b4 = np.float32(0.25)
        gamma = rng.standard_normal(D).astype(np.float32)
        beta = rng.standard_normal(D).astype(np.float32)
        head = self.m.EttinRerankHead(W2, W4, b4, gamma, beta)
        v = rng.standard_normal(D).astype(np.float32)
        # independent reference
        h = self.m.gelu(v @ W2.T)
        h = (h - h.mean()) / (h.std() + 1e-5) * gamma + beta
        ref = float((h @ W4.T + b4).reshape(-1)[0])
        self.assertAlmostEqual(head.score(v), ref, places=4)

    def test_no_layernorm_path(self):
        D = 4
        W2 = np.eye(D, dtype=np.float32)
        W4 = np.ones((1, D), dtype=np.float32)
        head = self.m.EttinRerankHead(W2, W4, b4=None)  # gamma/beta None -> skip LN
        v = np.array([1.0, 2.0, 3.0, 4.0], np.float32)
        ref = float((self.m.gelu(v) @ W4.T).reshape(-1)[0])
        self.assertAlmostEqual(head.score(v), ref, places=4)

    def test_batch_matches_single(self):
        rng = np.random.default_rng(1)
        D = 6
        head = self.m.EttinRerankHead(
            rng.standard_normal((D, D)).astype(np.float32),
            rng.standard_normal((1, D)).astype(np.float32),
            np.float32(0.0),
            rng.standard_normal(D).astype(np.float32),
            rng.standard_normal(D).astype(np.float32),
        )
        vs = rng.standard_normal((5, D)).astype(np.float32)
        batch = head.score(vs)
        self.assertEqual(batch.shape, (5,))
        for i in range(5):
            self.assertAlmostEqual(float(batch[i]), head.score(vs[i]), places=4)

    def test_dim_mismatch_raises(self):
        head = self.m.EttinRerankHead(np.eye(4, dtype=np.float32), np.ones((1, 4), np.float32), None)
        with self.assertRaises(ValueError):
            head.score(np.zeros(3, np.float32))

    def test_bad_shapes_raise(self):
        with self.assertRaises(ValueError):
            self.m.EttinRerankHead(np.zeros((4, 3), np.float32), np.ones((1, 4), np.float32), None)
        with self.assertRaises(ValueError):
            self.m.EttinRerankHead(np.eye(4, dtype=np.float32), np.ones((1, 5), np.float32), None)


@unittest.skipUnless(HAVE_NUMPY, "numpy required")
class Rerank(unittest.TestCase):
    def setUp(self):
        self.m = _load_module()

    def _head(self, D=4):
        # W2=I, no LN, W4 picks dim 0 — so score == GELU(v)[0]; a doc whose embed has
        # a bigger first component ranks higher. Lets us control ordering in the test.
        W4 = np.zeros((1, D), np.float32)
        W4[0, 0] = 1.0
        return self.m.EttinRerankHead(np.eye(D, dtype=np.float32), W4, None)

    def test_rerank_orders_descending_and_uses_pairs(self):
        head = self._head(4)
        seen = {}

        def embed_pairs(texts):
            seen["texts"] = texts
            # doc index encoded in first component: doc0 -> 0.1, doc1 -> 5.0, doc2 -> 1.0
            firsts = {0: 0.1, 1: 5.0, 2: 1.0}
            return np.array([[firsts[i], 0, 0, 0] for i in range(len(texts))], np.float32)

        res = head.rerank("q", ["d0", "d1", "d2"], embed_pairs)
        self.assertEqual([r["index"] for r in res], [1, 2, 0])  # by descending score
        self.assertEqual(seen["texts"], ["q</s>d0", "q</s>d1", "q</s>d2"])  # pair format
        self.assertGreater(res[0]["relevance_score"], res[1]["relevance_score"])

    def test_empty_docs(self):
        self.assertEqual(self._head().rerank("q", [], lambda t: None), [])

    def test_single_doc(self):
        head = self._head()
        res = head.rerank("q", ["only"], lambda t: np.array([[2.0, 0, 0, 0]], np.float32))
        self.assertEqual(len(res), 1)
        self.assertEqual(res[0]["index"], 0)


@unittest.skipUnless(HAVE_NUMPY, "numpy required")
class FromDir(unittest.TestCase):
    def test_load_from_safetensors_dir(self):
        try:
            from safetensors.numpy import save_file
        except ImportError:
            self.skipTest("safetensors required")
        m = _load_module()
        D = 5
        rng = np.random.default_rng(2)
        d = tempfile.mkdtemp()
        os.makedirs(os.path.join(d, "2_Dense"))
        os.makedirs(os.path.join(d, "3_LayerNorm"))
        os.makedirs(os.path.join(d, "4_Dense"))
        W2 = rng.standard_normal((D, D)).astype(np.float32)
        W4 = rng.standard_normal((1, D)).astype(np.float32)
        b4 = rng.standard_normal(1).astype(np.float32)
        g = rng.standard_normal(D).astype(np.float32)
        b = rng.standard_normal(D).astype(np.float32)
        save_file({"linear.weight": W2}, os.path.join(d, "2_Dense", "model.safetensors"))
        save_file({"linear.weight": W4, "linear.bias": b4}, os.path.join(d, "4_Dense", "model.safetensors"))
        save_file({"norm.weight": g, "norm.bias": b}, os.path.join(d, "3_LayerNorm", "model.safetensors"))
        head = m.EttinRerankHead.from_dir(d)
        self.assertEqual(head.dim, D)
        v = rng.standard_normal(D).astype(np.float32)
        direct = m.EttinRerankHead(W2, W4, b4, g, b)
        self.assertAlmostEqual(head.score(v), direct.score(v), places=5)

        # npz round-trip (the runtime form baked into the image); from_dir prefers it.
        npz = os.path.join(d, "head.npz")
        direct.to_npz(npz)
        self.assertAlmostEqual(m.EttinRerankHead.from_npz(npz).score(v), direct.score(v), places=5)
        self.assertAlmostEqual(m.EttinRerankHead.from_dir(d).score(v), direct.score(v), places=5)


if __name__ == "__main__":
    unittest.main()
