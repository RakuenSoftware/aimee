#!/usr/bin/env python3
"""Focused tests for the provider-registration gate.

The gate exists because a null provider is silent: the pointer is never set, the
path fails closed, and no test notices because every test registers its own. A
gate against that failure mode must not be able to fail the same way itself --
so these cover the passing case, the failing case, both directions of the
UNREACHABLE ledger, the narrowing that keeps ordinary in-process hooks out, and
the vacuous-pass guard.
"""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "scripts" / "check_provider_registration.py"

SPEC = importlib.util.spec_from_file_location("provider_registration_checker", SOURCE)
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)

CMAKE = """
set(KB_SRCS
    ${AIMEE_SRC_DIR}/modules/thing/thing.c
)
set(SERVER_SRCS
    ${AIMEE_SRC_DIR}/modules/other/other.c
)
"""

# The consumer: it owns the pointer, and goes null without a registration.
THING_C = """
static thing_fn g_thing;
void thing_register_provider(thing_fn provider)
{
   g_thing = provider;
}
int thing_do(void)
{
   return g_thing ? g_thing() : -1;
}
"""

OTHER_C = """
static other_fn g_other;
void other_register_provider(other_fn provider)
{
   g_other = provider;
}
"""


class ProviderRegistrationTest(unittest.TestCase):
    def build_tree(self, kb_adapter: str, server_adapter: str, extra: dict | None = None):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        src = root / "src"
        (src / "modules/thing").mkdir(parents=True)
        (src / "modules/other").mkdir(parents=True)
        (src / "kb").mkdir(parents=True)
        (src / "server").mkdir(parents=True)
        (src / "modules/thing/thing.c").write_text(THING_C, encoding="utf-8")
        (src / "modules/other/other.c").write_text(OTHER_C, encoding="utf-8")
        (src / "kb/adapter.c").write_text(kb_adapter, encoding="utf-8")
        (src / "server/adapter.c").write_text(server_adapter, encoding="utf-8")
        for name, body in (extra or {}).items():
            path = src / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(body, encoding="utf-8")
        return root

    def analyse(self, root, unreachable=None, cmake=CMAKE):
        adapters = {"kb": root / "src/kb/adapter.c", "server": root / "src/server/adapter.c"}
        lists = {"kb": ("KB_SRCS",), "server": ("SERVER_SRCS",)}
        return CHECKER.analyse(root, cmake, adapters, lists, unreachable or {})

    def test_registered_in_both_passes(self):
        tree = self.build_tree("void c(void){ thing_register_provider(f); }",
                               "void c(void){ thing_register_provider(f); "
                               "other_register_provider(g); }")
        code, messages = self.analyse(tree)
        self.assertEqual(code, 0, messages)

    def test_a_daemon_that_builds_the_consumer_and_never_registers_fails(self):
        """The exact shape that shipped: the server registers, the KB does not."""
        tree = self.build_tree("void c(void){ /* nothing */ }",
                               "void c(void){ thing_register_provider(f); "
                               "other_register_provider(g); }")
        code, messages = self.analyse(tree)
        self.assertEqual(code, 1)
        joined = "\n".join(messages)
        self.assertIn("kb builds modules/thing/thing.c", joined)
        self.assertIn("thing_register_provider", joined)

    def test_an_unreachable_entry_excuses_it(self):
        tree = self.build_tree("void c(void){ /* nothing */ }",
                               "void c(void){ thing_register_provider(f); "
                               "other_register_provider(g); }")
        code, messages = self.analyse(tree, {("kb", "thing_register_provider"): "unreachable here"})
        self.assertEqual(code, 0, messages)

    def test_a_stale_unreachable_entry_is_reported(self):
        """An entry that outlived its reason would forgive a real regression."""
        tree = self.build_tree("void c(void){ thing_register_provider(f); }",
                               "void c(void){ thing_register_provider(f); "
                               "other_register_provider(g); }")
        code, messages = self.analyse(tree, {("kb", "thing_register_provider"): "stale"})
        self.assertEqual(code, 1)
        self.assertIn("no longer apply", "\n".join(messages))

    def test_a_registrar_no_adapter_mentions_is_out_of_scope(self):
        """Ordinary in-process hooks -- tool tables, block executors, config
        reappliers -- are wired up elsewhere and are not this gate's business."""
        tree = self.build_tree("void c(void){ thing_register_provider(f); }",
                               "void c(void){ thing_register_provider(f); }")
        code, messages = self.analyse(tree)
        # other_register_provider is owned by a file the server builds and is
        # registered by neither adapter, so it must be ignored, not reported.
        self.assertEqual(code, 0, messages)

    def test_missing_adapter_is_a_hard_error(self):
        tree = self.build_tree("void c(void){ thing_register_provider(f); }",
                               "void c(void){ thing_register_provider(f); }")
        (tree / "src/kb/adapter.c").unlink()
        code, _ = self.analyse(tree)
        self.assertEqual(code, 2)

    def test_moved_cmake_lists_are_a_hard_error(self):
        tree = self.build_tree("void c(void){ thing_register_provider(f); }",
                               "void c(void){ thing_register_provider(f); }")
        code, messages = self.analyse(tree, cmake="set(SOMETHING_ELSE\n)\n")
        self.assertEqual(code, 2)
        self.assertIn("CMake list names have moved", "\n".join(messages))

    def test_a_gate_that_sees_nothing_does_not_report_success(self):
        """If the registrar spelling drifts, every pair vanishes. Reporting ok
        there is how a guard quietly stops guarding."""
        tree = self.build_tree("void c(void){ renamed_hook_install(f); }",
                               "void c(void){ renamed_hook_install(f); }")
        code, messages = self.analyse(tree)
        self.assertEqual(code, 2)
        self.assertIn("no longer checking anything", "\n".join(messages))

    def test_the_real_tree_passes(self):
        """The committed configuration must hold, or the gate is already broken."""
        code, messages = CHECKER.analyse(CHECKER.ROOT, CHECKER.CMAKE, CHECKER.ADAPTERS,
                                         CHECKER.SOURCE_LISTS, CHECKER.UNREACHABLE)
        self.assertEqual(code, 0, "\n".join(messages))

    def test_deleting_the_registration_reproduces_the_defect_that_shipped(self):
        """The bug this gate was written for, against the real tree: remove the
        KB's signal-classifier registration and the gate must report it."""
        adapter = CHECKER.ADAPTERS["kb"]
        original = adapter.read_text(encoding="utf-8")
        broken = "\n".join(l for l in original.splitlines()
                           if "learning_router_register_signal_classifier" not in l)
        self.assertNotEqual(original, broken, "the registration line has moved")
        try:
            adapter.write_text(broken + "\n", encoding="utf-8")
            code, messages = CHECKER.analyse(CHECKER.ROOT, CHECKER.CMAKE, CHECKER.ADAPTERS,
                                             CHECKER.SOURCE_LISTS, CHECKER.UNREACHABLE)
        finally:
            adapter.write_text(original, encoding="utf-8")
        self.assertEqual(code, 1)
        self.assertIn("learning_router_register_signal_classifier", "\n".join(messages))


if __name__ == "__main__":
    unittest.main()
