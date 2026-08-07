"""Nothing may return out of kb_process_one_file after the chunks are committed.

WHY THIS EXISTS. The tail of kb_process_one_file is per-file bookkeeping:

    sketch_bloom_add_hash / count_min / hll     -- dedup sketches
    kb_minhash_upsert_fenced                    -- near-duplicate signature
    kb_file_index_store_from_path               -- THE file index
    c->stats->files_indexed++                   -- the progress counter

Skipping the dense vector for source files was implemented as an early return
once the chunk rows were committed. It looked local and safe. It was not: the
tail above never ran for any non-prose file, and that is not a cosmetic loss.

  * A file with no kb_file_index row can never be skipped by the mtime check at
    the top of this function, so EVERY non-prose file was re-chunked on EVERY
    build, forever -- the opposite of the change's purpose.
  * files_indexed stopped counting, so a real pass reported
    "2284 files scanned, 0 indexed, 994 skipped, 7683 chunks" -- zero indexed
    while adding 7683 chunks, a contradiction that took a full corpus rebuild
    to run down.
  * Worse, it was silently ASYMMETRIC. Prose files still completed the tail, so
    their recorded ingest time outran their mtime; on the next build all 173 of
    them were skipped as unchanged even though their chunks had been deleted.
    The corpus came back with 7683 source chunks and no prose at all.

The general rule is what the test enforces: once phase 1 has committed rows,
this function owns bookkeeping that MUST run, so it has exactly one exit. Any
future "just skip this file's expensive part" needs to gate the work, not
return past the tail.
"""
import pathlib
import re
import unittest

KB_C = pathlib.Path(__file__).resolve().parents[2] / "src" / "kb" / "kb.c"
FUNC = "static void kb_process_one_file"
# Phase 1 commits the chunk rows; everything after this marker runs with rows
# already durable, which is what makes an early exit lossy.
PHASE2 = "/* Phase 2: embed each committed chunk"
TAIL_CALL = "kb_file_index_store_from_path"


def function_body(src, signature):
    """Return the text of the function starting at `signature`, brace-matched."""
    start = src.index(signature)
    open_brace = src.index("{", start)
    depth = 0
    for i in range(open_brace, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[start:i + 1]
    raise AssertionError("unbalanced braces in %s" % signature)


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


class TestSingleExitAfterCommit(unittest.TestCase):
    def setUp(self):
        self.body = function_body(KB_C.read_text(), FUNC)

    def test_the_tail_bookkeeping_is_still_there(self):
        """Guards the test itself: if the tail moves, this test is meaningless."""
        self.assertIn(TAIL_CALL, self.body,
                      "%s no longer calls %s -- this test needs rewriting, not deleting"
                      % (FUNC, TAIL_CALL))
        self.assertIn(PHASE2, self.body, "phase 2 marker comment is gone")

    def test_no_return_after_the_chunks_are_committed(self):
        after = strip_comments(self.body[self.body.index(PHASE2):])
        offenders = [m.start() for m in re.finditer(r"\breturn\b", after)]
        self.assertEqual(
            offenders, [],
            "kb_process_one_file returns after committing chunks, so it skips "
            "%s and the dedup sketches for that file. Gate the work in an "
            "`if`, do not return past the tail." % TAIL_CALL)

    def test_bookkeeping_is_not_itself_conditional(self):
        """The tail must run for every file that reached it."""
        tail = strip_comments(self.body[self.body.index(TAIL_CALL):])
        self.assertNotIn("if (", tail.split(TAIL_CALL)[0],
                         "the file-index store became conditional")


if __name__ == "__main__":
    unittest.main()
