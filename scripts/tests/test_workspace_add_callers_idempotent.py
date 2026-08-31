"""No caller of config_workspace_add may treat -2 (already registered) as fatal.

The contract test in src/tests/test_workspace_add_idempotent.c pins what
config_workspace_add RETURNS. It cannot catch the bug that actually bit, which
lived in the callers: both the workspace.add RPC handler and the CLI turned -2
into a hard error, so `workspace add` was not idempotent and every re-run of any
automation that registers its workspace died at setup.

That bug also had a second half that is easy to reintroduce. Removing the
explicit `if (add_rc == -2) return error;` is not sufficient, because the
catch-all below it

    if (add_rc != 0)
        return error;

still swallows -2. Fixing only the first line looks correct and changes nothing.
This test requires the catch-all to exclude -2 as well -- which is exactly the
step missed the first time.
"""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]

# Every site that calls config_workspace_add and decides what to do with it.
CALL_SITES = [
    ROOT / "src" / "server" / "server_runner_endpoints.c",
    ROOT / "src" / "cmd_infra.c",
]


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def region_after_call(text, span=2000):
    """The decision block right after config_workspace_add, comments removed."""
    i = text.index("config_workspace_add(")
    return strip_comments(text[i:i + span])


class TestCallersTreatDuplicateAsSuccess(unittest.TestCase):
    def test_call_sites_are_where_we_think(self):
        """Guards the test: if a call site moves, fail loudly, don't pass vacuously."""
        for path in CALL_SITES:
            self.assertTrue(path.is_file(), "%s is gone -- update this test" % path)
            self.assertIn("config_workspace_add(", path.read_text(),
                          "%s no longer calls config_workspace_add" % path)

    def test_minus_two_is_not_returned_as_an_error(self):
        for path in CALL_SITES:
            region = region_after_call(path.read_text())
            m = re.search(r"add_rc\s*==\s*-2\s*\)\s*\n?\s*return", region)
            self.assertIsNone(
                m,
                "%s returns early on add_rc == -2. Registering an already "
                "registered workspace is the state the caller asked for; it "
                "must not be an error." % path.name)

    def test_the_catch_all_excludes_minus_two(self):
        """The half that was missed: `if (add_rc != 0)` still swallows -2."""
        for path in CALL_SITES:
            region = region_after_call(path.read_text())
            for m in re.finditer(r"add_rc\s*!=\s*0([^)\n]*)\)", region):
                self.assertIn(
                    "-2", m.group(1),
                    "%s has `if (add_rc != 0%s)` which still treats -2 as a "
                    "failure. It must exclude -2 (e.g. `&& add_rc != -2`), or "
                    "removing the explicit -2 branch changes nothing."
                    % (path.name, m.group(1)))


if __name__ == "__main__":
    unittest.main()
