"""Concurrent small requests must reach the model as ONE call.

WHY. Batch size dominates this model's cost, measured on the bench host:

    n=1   58.7 ms/text        n=32   13.0 ms/text
    n=8   15.3 ms/text        n=128  11.7 ms/text

and the kb's document ingest cannot batch well by construction -- it embeds one
file at a time and a source file is usually 1-5 chunks. Six ingest workers each
sending its own tiny batch therefore paid the n=1 penalty on nearly every chunk:
observed ~40 vectors/min against a measured capability of ~190, with the embedder
process pegged at 527% CPU doing mostly per-call overhead.

Coalescing fixes it for every caller at once. The property under test is the one
that matters -- CALLS TO THE MODEL, not wall-clock -- because that is what the
5x is paid on, and asserting on timing would be flaky under load.
"""
import importlib.util
import os
import pathlib
import sys
import threading
import unittest

MODULE = pathlib.Path(__file__).resolve().parents[2] / "scripts" / "embedder-server.py"


def load(window_ms="50", batch_max="128"):
    """Import embedder-server with a stub model that records每 call size."""
    os.environ["EMBEDDER_BATCH_WINDOW_MS"] = window_ms
    os.environ["EMBEDDER_BATCH_MAX"] = batch_max
    os.environ["EMBEDDER_MODEL"] = "stub"
    spec = importlib.util.spec_from_file_location("embedder_server_under_test", MODULE)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


class RecordingModel:
    """Stands in for the sentence-transformers model, recording batch sizes."""

    def __init__(self, dim=4):
        self.dim = dim
        self.calls = []
        self._lock = threading.Lock()

    def encode(self, texts, normalize_embeddings=True):
        single = isinstance(texts, str)
        if single:
            texts = [texts]
        with self._lock:
            self.calls.append(len(texts))

        class V(list):
            def tolist(self):
                return list(self)

        rows = [V([0.5] * self.dim) for _ in texts]
        # model.encode(str) returns ONE vector, not a list of them -- the
        # non-coalesced path relies on that shape.
        return rows[0] if single else rows


class TestDynamicBatching(unittest.TestCase):
    def setUp(self):
        self.mod = load()
        self.model = RecordingModel()
        self.mod._model = self.model
        self.mod.EMBEDDER_STUB = False
        self.mod.SPEC = None  # prefix_for returns "" without a registry entry

    def test_concurrent_single_embeds_become_one_model_call(self):
        """The ingest shape: many callers, one text each, all at once."""
        n = 24
        results = [None] * n
        barrier = threading.Barrier(n)

        def one(i):
            barrier.wait()  # release together so they overlap inside the window
            results[i] = self.mod.embed("chunk %d" % i)

        threads = [threading.Thread(target=one, args=(i,)) for i in range(n)]
        [t.start() for t in threads]
        [t.join() for t in threads]

        self.assertTrue(all(r is not None for r in results), "every caller must get a vector")
        self.assertEqual(sum(self.model.calls), n, "every text must be embedded exactly once")
        # The point of the change: far fewer model calls than callers.
        self.assertLess(len(self.model.calls), n,
                        "concurrent singles did not coalesce: %r" % (self.model.calls,))
        self.assertGreater(max(self.model.calls), 1,
                           "no batch larger than one was formed: %r" % (self.model.calls,))

    def test_batch_max_bounds_one_call(self):
        """A burst must not grow a single model call without limit."""
        self.mod.EMBEDDER_BATCH_MAX = 8
        n = 32
        barrier = threading.Barrier(n)

        def one(i):
            barrier.wait()
            self.mod.embed("chunk %d" % i)

        threads = [threading.Thread(target=one, args=(i,)) for i in range(n)]
        [t.start() for t in threads]
        [t.join() for t in threads]
        self.assertEqual(sum(self.model.calls), n)
        self.assertLessEqual(max(self.model.calls), 8,
                             "a single call exceeded EMBEDDER_BATCH_MAX: %r" % (self.model.calls,))

    def test_vectors_are_returned_to_the_right_caller(self):
        """Coalescing must not transpose results between callers."""
        dims = {}

        class Distinct(RecordingModel):
            def encode(self, texts, normalize_embeddings=True):
                if isinstance(texts, str):
                    texts = [texts]
                with self._lock:
                    self.calls.append(len(texts))

                class V(list):
                    def tolist(self):
                        return list(self)

                # Encode the text's own index into its vector.
                return [V([float(int(t.split()[-1]))] * self.dim) for t in texts]

        self.mod._model = Distinct()
        n = 16
        out = [None] * n
        barrier = threading.Barrier(n)

        def one(i):
            barrier.wait()
            out[i] = self.mod.embed("chunk %d" % i)

        threads = [threading.Thread(target=one, args=(i,)) for i in range(n)]
        [t.start() for t in threads]
        [t.join() for t in threads]
        for i in range(n):
            self.assertEqual(out[i][0], float(i),
                             "caller %d received another caller's vector: %r" % (i, out[i]))
        dims.clear()

    def test_window_zero_disables_coalescing(self):
        """The escape hatch must actually bypass the batcher."""
        mod = load(window_ms="0")
        model = RecordingModel()
        mod._model = model
        mod.EMBEDDER_STUB = False
        mod.SPEC = None
        for i in range(4):
            mod.embed("chunk %d" % i)
        self.assertEqual(model.calls, [1, 1, 1, 1])


if __name__ == "__main__":
    unittest.main()
