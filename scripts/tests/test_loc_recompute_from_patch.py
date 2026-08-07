"""Pin the test/production split used to restate the benchmark's LOC figures.

RED-GREEN NOTE. codex_matrix_runner.py buckets a changed file as test only when
its path starts with "tests/". The am_ corpus keeps its tests under src/tests/,
so the prefix never matched, every test file counted as production, and
test_added was structurally zero in all 25 cells across every arm.

That was not a small miscount. It produced a published claim -- "no arm wrote
tests on a corpus built from real fix commits" -- that is the opposite of the
truth: every arm wrote tests on most tasks, and on am_1f0f1ab528 aimee was the
ONLY arm that wrote one, at production parity with baseline. It also inflated
every production_added figure by whatever test code that cell wrote, which is
precisely the axis the article set out to compare.

test_startswith_prefix_is_the_original_bug below fails against that rule and
passes against is_test(), so the regression cannot come back silently.
"""
import importlib.util
import pathlib
import unittest

_MODULE_PATH = (pathlib.Path(__file__).resolve().parents[2]
                / "benchmarks" / "loc_recompute_from_patch.py")


def _load():
    """Import the recompute script without running its __main__ body.

    The script walks /opt/bench at import time on the bench host, so pull the
    one pure function out rather than executing the module.
    """
    src = _MODULE_PATH.read_text()
    marker = "rows = []"
    body = src[:src.index(marker)] if marker in src else src
    ns = {"__name__": "loc_recompute_under_test"}
    exec(compile(body, str(_MODULE_PATH), "exec"), ns)
    return ns


class TestIsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.is_test = staticmethod(_load()["is_test"])

    def test_src_tests_is_where_this_corpus_keeps_its_tests(self):
        # The exact case the runner got wrong, and the reason every cell
        # reported test_added: 0.
        self.assertTrue(self.is_test("src/tests/test_bus_capture.c"))
        self.assertTrue(self.is_test("src/tests/test_kb_client_index.c"))

    def test_top_level_tests_dir_still_counts(self):
        self.assertTrue(self.is_test("tests/test_smoke.py"))

    def test_test_prefixed_basename_counts_anywhere(self):
        self.assertTrue(self.is_test("some/deep/path/test_thing.c"))

    def test_go_table_tests_count(self):
        self.assertTrue(self.is_test("control-web/acl_drift_test.go"))

    def test_production_files_are_not_tests(self):
        self.assertFalse(self.is_test("src/dstr.c"))
        self.assertFalse(self.is_test("src/modules/bus/bus_route.c"))
        # "latest" contains "test" but is not a test path; substring matching
        # on the bare word would misfile it.
        self.assertFalse(self.is_test("src/latest_manifest.c"))

    def test_startswith_prefix_is_the_original_bug(self):
        """The runner's rule, stated directly, and why it was wrong.

        Red against the original implementation: it buckets this corpus's tests
        as production. Green against is_test().
        """
        original_rule = lambda rel: rel.startswith("tests/")  # noqa: E731
        corpus_test = "src/tests/test_bus_capture.c"
        self.assertFalse(original_rule(corpus_test),
                         "if this passes the original rule was never broken")
        self.assertTrue(self.is_test(corpus_test))


if __name__ == "__main__":
    unittest.main()
