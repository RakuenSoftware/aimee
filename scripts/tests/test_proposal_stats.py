#!/usr/bin/env python3
"""Tests for scripts/proposal_stats.py word-count edge cases."""
from __future__ import annotations

import importlib.util
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT_PATH = os.path.normpath(os.path.join(HERE, "..", "proposal_stats.py"))


def _load_module():
    spec = importlib.util.spec_from_file_location("proposal_stats", SCRIPT_PATH)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class _Capture:
    def __init__(self):
        self.err = io.StringIO()
        self.out = io.StringIO()

    def __enter__(self):
        sys.stderr = self.err
        sys.stdout = self.out
        return self

    def __exit__(self, *_):
        sys.stderr = sys.__stderr__
        sys.stdout = sys.__stdout__


class CountWordsChunkBoundary(unittest.TestCase):
    """Regression tests for words and UTF-8 chars split across 64 KiB reads."""

    CHUNK = 64 * 1024

    def setUp(self):
        self.mod = _load_module()
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _write(self, name: str, data: bytes) -> str:
        path = os.path.join(self.tmp, name)
        with open(path, "wb") as fh:
            fh.write(data)
        return path

    def test_ascii_word_straddling_chunk_boundary(self):
        # 'foo' starts 3 bytes from end of the first 64 KiB chunk.
        pre = b"a" * (self.CHUNK - 3)
        text = pre + b"foo bar baz\n"
        path = self._write("ascii.md", text)
        with _Capture():
            count = self.mod._count_words([path])
        # 3 tokens: foo, bar, baz
        self.assertEqual(count, 3)

    def test_multibyte_utf8_char_straddling_chunk_boundary(self):
        # '€' is 3 bytes: \xe2\x82\xac. Split it so 1 byte lands at the
        # end of chunk 1 and the remaining 2 bytes land at the start of
        # chunk 2. The decoder must buffer the incomplete sequence rather
        # than raising UnicodeDecodeError.
        pre = b"x" * (self.CHUNK - 1)
        text = pre + b"\xe2" + b"\x82\xac rest alpha\n"
        path = self._write("utf8.md", text)
        with _Capture():
            count = self.mod._count_words([path])
        # 3 tokens: €, rest, alpha
        self.assertEqual(count, 3)

    def test_word_split_inside_file_no_double_count(self):
        # Place 'one' entirely at the boundary: nothing before, just a token
        # that starts near the end of chunk 1 and finishes in chunk 2.
        pre = b"z" * (self.CHUNK - 2)
        text = pre + b"ab" + b"cd ef\n"
        path = self._write("split.md", text)
        with _Capture():
            count = self.mod._count_words([path])
        # abcd counts as one word straddling boundary; ef second. Total 2.
        self.assertEqual(count, 2)

    def test_binary_file_skipped(self):
        # A NUL byte triggers the binary-warning path.
        text = b"hello\x00world"
        path = self._write("bin.md", text)
        with _Capture():
            count = self.mod._count_words([path])
        self.assertEqual(count, 0)

    def test_invalid_utf8_skipped(self):
        # A lone continuation byte is not valid UTF-8 anywhere.
        text = b"hello\x80world"
        path = self._write("bad.md", text)
        with _Capture():
            count = self.mod._count_words([path])
        self.assertEqual(count, 0)


class CollectIntegration(unittest.TestCase):
    def setUp(self):
        self.mod = _load_module()
        self.root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.root, True)
        # Redirect the script's hard-coded root to our temp tree so we can
        # exercise _collect / main without touching the real proposals dir.
        self._real_root = self.mod._proposals_root
        self.mod._proposals_root = lambda: self.root
        self.addCleanup(
            setattr, self.mod, "_proposals_root", self._real_root
        )

    def _setup_root(self, pending: list[bytes], done: list[bytes]) -> None:
        os.makedirs(os.path.join(self.root, "pending"))
        os.makedirs(os.path.join(self.root, "done"))
        for i, data in enumerate(pending):
            with open(
                os.path.join(self.root, "pending", f"p{i}.md"), "wb"
            ) as fh:
                fh.write(data)
        for i, data in enumerate(done):
            with open(
                os.path.join(self.root, "done", f"d{i}.md"), "wb"
            ) as fh:
                fh.write(data)

    def test_collect_counts_correctly_with_boundary_token(self):
        pre = b"a" * (self.mod._READ_CHUNK_BYTES - 4)
        self._setup_root(
            pending=[pre + b"alpha beta\n"],
            done=[b"done file words here\n"],
        )
        with _Capture():
            stats = self.mod._collect(self.root)
        self.assertEqual(stats["pending"], 1)
        self.assertEqual(stats["done"], 1)
        self.assertEqual(stats["pending_words"], 2)

    def test_collect_json_round_trip(self):
        self._setup_root(pending=[b"one two three\n"], done=[b"x\n"])
        cap = _Capture()
        with cap:
            rc = self.mod.main(["--json"])
        self.assertEqual(rc, 0)
        payload = cap.out.getvalue().strip()
        parsed = json.loads(payload)
        self.assertEqual(parsed["pending"], 1)
        self.assertEqual(parsed["done"], 1)
        self.assertEqual(parsed["pending_words"], 3)


if __name__ == "__main__":
    unittest.main()