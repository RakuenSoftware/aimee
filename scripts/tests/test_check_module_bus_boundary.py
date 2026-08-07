#!/usr/bin/env python3
"""Mutation tests for the module bus boundary."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


REPO = Path(__file__).resolve().parents[2]
CHECKER_PATH = REPO / "scripts/check_module_bus_boundary.py"
SPEC = importlib.util.spec_from_file_location("module_bus_boundary", CHECKER_PATH)
assert SPEC and SPEC.loader
checker = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = checker
SPEC.loader.exec_module(checker)


# A miniature module tree: `tools` reaches `workspace` in process, and that one
# crossing is the whole declared debt. Everything else must stay clean.
CROSSING = ("src/modules/tools/agent_tools.c", "aimee/workspace/workspace.h")
FIXTURE = {
    "src/modules/tools/agent_tools.c": (
        "#include <aimee/tools/agent_tools.h>\n"
        "#include <aimee/core/bus_client.h>\n"
        "#include <aimee/workspace/workspace.h>\n"
    ),
    "src/modules/tools/include/aimee/tools/agent_tools.h": "#include <aimee/core/bus_client.h>\n",
    # A private header at the module root: the shape the build puts on the
    # include path, so a peer can name it without any prefix.
    "src/modules/tools/tools_internal.h": "#pragma once\n",
    "src/modules/workspace/workspace.c": "#include <aimee/core/bus_client.h>\n",
    "src/modules/workspace/include/aimee/workspace/workspace.h": "#pragma once\n",
}


class ModuleBusBoundaryTests(unittest.TestCase):
    def fixture(self, root: Path, files: dict[str, str] | None = None) -> None:
        for relative, body in (files or FIXTURE).items():
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(body, encoding="utf-8")

    def assert_fixture(self, mutate, rule: str | None, allowed=frozenset({CROSSING})) -> None:
        """Build the miniature tree, mutate it, and assert the checker's ruling."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            mutate(root)
            with mock.patch.object(checker, "ALLOWED", set(allowed)):
                if rule is None:
                    checker.validate(root)
                else:
                    with self.assertRaisesRegex(checker.CheckError, f"rule={rule}"):
                        checker.validate(root)

    def append(self, relative: str, line: str):
        def mutate(root: Path) -> None:
            target = root / relative
            target.write_text(target.read_text(encoding="utf-8") + line, encoding="utf-8")
        return mutate

    def test_repository_and_atomic_fixture_pass(self) -> None:
        checker.validate(REPO)
        self.assert_fixture(lambda root: None, None)

    def test_a_new_peer_include_is_rejected(self) -> None:
        self.assert_fixture(
            self.append("src/modules/workspace/workspace.c", "#include <aimee/tools/agent_tools.h>\n"),
            "undeclared-cross-module",
        )

    def test_a_second_peer_header_in_an_allowlisted_file_is_rejected(self) -> None:
        """The debt is keyed by (path, header), not by the module pair."""
        self.assert_fixture(
            self.append("src/modules/tools/agent_tools.c", "#include <aimee/workspace/manifest.h>\n"),
            "undeclared-cross-module",
        )

    def test_removing_a_declared_crossing_is_rejected(self) -> None:
        def mutate(root: Path) -> None:
            target = root / CROSSING[0]
            target.write_text(
                target.read_text(encoding="utf-8").replace(f"#include <{CROSSING[1]}>\n", ""),
                encoding="utf-8",
            )
        self.assert_fixture(mutate, "stale-allowlist")

    def test_core_includes_never_trip_the_check(self) -> None:
        self.assert_fixture(
            self.append("src/modules/workspace/workspace.c", "#include <aimee/core/module_host.h>\n"),
            None,
        )

    def test_bus_transport_headers_never_trip_the_check(self) -> None:
        for header in sorted(checker.BUS_TRANSPORT_HEADERS):
            with self.subTest(header=header):
                self.assert_fixture(
                    self.append("src/modules/workspace/workspace.c", f"#include <{header}>\n"),
                    None,
                )

    def test_a_peer_domain_header_beside_a_transport_header_is_still_rejected(self) -> None:
        """obs_bus.h is exempt; the audit module's domain API is not."""
        self.assert_fixture(
            self.append(
                "src/modules/workspace/workspace.c",
                "#include <aimee/audit/obs_bus.h>\n#include <aimee/audit/audit_worm.h>\n",
            ),
            "undeclared-cross-module",
        )

    def test_self_includes_never_trip_the_check(self) -> None:
        self.assert_fixture(
            self.append("src/modules/workspace/workspace.c", "#include <aimee/workspace/manifest.h>\n"),
            None,
        )

    def test_a_public_header_is_owned_by_its_module_not_its_include_path(self) -> None:
        """src/modules/X/include/aimee/X/y.h is X's own, but a peer root is not."""
        self.assert_fixture(
            self.append(
                "src/modules/workspace/include/aimee/workspace/workspace.h",
                "#include <aimee/workspace/handle.h>\n",
            ),
            None,
        )
        self.assert_fixture(
            self.append(
                "src/modules/workspace/include/aimee/workspace/workspace.h",
                "#include <aimee/tools/agent_tools.h>\n",
            ),
            "undeclared-cross-module",
        )

    def test_a_commented_out_include_is_not_a_crossing(self) -> None:
        self.assert_fixture(
            self.append(
                "src/modules/workspace/workspace.c",
                "/* #include <aimee/tools/agent_tools.h> */\n",
            ),
            None,
        )

    def test_a_bare_include_of_a_peers_header_is_rejected(self) -> None:
        """The build puts module directories on the include path, so a bare name
        reaches straight into a peer. Counting only prefixed includes missed 122
        crossings against the 45 it saw."""
        self.assert_fixture(
            self.append("src/modules/workspace/workspace.c", '#include "tools_internal.h"\n'),
            "undeclared-cross-module",
        )

    def test_a_bare_include_resolved_by_the_own_directory_is_not_a_crossing(self) -> None:
        """A quoted include finds the including file's own directory first.

        Two module roots sharing a basename is ambiguity and is refused outright,
        so the case this rule actually covers is a file in a SUBdirectory sitting
        beside its own copy of a name some module root also publishes.
        """
        def mutate(root: Path) -> None:
            nested = root / "src/modules/workspace/sub"
            nested.mkdir(parents=True, exist_ok=True)
            (nested / "tools_internal.h").write_text("#pragma once\n", encoding="utf-8")
            (nested / "nested.c").write_text('#include "tools_internal.h"\n', encoding="utf-8")
        self.assert_fixture(mutate, None)

    def test_a_bare_include_naming_no_module_is_ignored(self) -> None:
        self.assert_fixture(
            self.append("src/modules/workspace/workspace.c", '#include "nothing_owns_this.h"\n'),
            None,
        )

    def test_an_ambiguous_module_header_name_is_refused(self) -> None:
        """Two modules publishing one basename makes ownership depend on include
        order, so the check refuses to guess rather than pick the first."""
        def mutate(root: Path) -> None:
            (root / "src/modules/workspace/tools_internal.h").write_text("#pragma once\n", encoding="utf-8")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            (root / "src/modules/tools/tools_internal.h").write_text("#pragma once\n", encoding="utf-8")
            mutate(root)
            with self.assertRaisesRegex(checker.CheckError, "rule=ambiguous-module-header"):
                checker.crossings(root)

    def test_a_module_header_shadowed_by_src_headers_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            (root / "src/headers").mkdir(parents=True, exist_ok=True)
            (root / "src/headers/workspace.h").write_text("#pragma once\n", encoding="utf-8")
            (root / "src/modules/workspace/workspace.h").write_text("#pragma once\n", encoding="utf-8")
            with self.assertRaisesRegex(checker.CheckError, "rule=shadowed-module-header"):
                checker.crossings(root)

    def test_core_linked_reaches_name_a_core_linked_module(self) -> None:
        """The exception is only meaningful if the group actually holds it."""
        core_linked = {"audit", "config", "execution-policy", "gateway", "ir",
                       "module-runtime", "protocols", "translation", "vault"}
        for path, header in checker.CORE_LINKED_REACH:
            with self.subTest(path=path):
                self.assertIn(header.split("/")[0], core_linked)
        for path, header in checker.FLAT_ROOT_REACH:
            with self.subTest(path=path):
                self.assertNotIn(header.split("/")[0], core_linked)

    def test_a_missing_module_root_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(checker.CheckError, "rule=module-root-missing"):
                checker.validate(Path(tmp))

    def test_a_quoted_include_couples_exactly_as_hard_as_an_angled_one(self) -> None:
        self.assert_fixture(
            self.append('src/modules/workspace/workspace.c', '#include "aimee/tools/agent_tools.h"\n'),
            "undeclared-cross-module",
        )

    def test_reaching_a_peers_private_tree_is_rejected(self) -> None:
        """`modules/<id>/` bypasses the peer's public API entirely."""
        self.assert_fixture(
            self.append('src/modules/workspace/workspace.c', '#include "modules/tools/tools_internal.h"\n'),
            "undeclared-cross-module",
        )

    def test_a_modules_include_of_the_owners_own_tree_is_not_a_crossing(self) -> None:
        self.assert_fixture(
            self.append('src/modules/workspace/workspace.c', '#include "modules/workspace/handle.h"\n'),
            None,
        )

    def test_lower_layers_are_not_peers(self) -> None:
        """db1/, db2/ and bare filenames name no module and are none of our business."""
        for include in ('"db1/user_memory.h"', '"db2/artifacts.h"', '"local_helper.h"',
                        '<stdio.h>', '"headers/util.h"'):
            with self.subTest(include=include):
                self.assert_fixture(
                    self.append("src/modules/workspace/workspace.c", f"#include {include}\n"),
                    None,
                )

    def test_every_declared_crossing_is_classified_exactly_once(self) -> None:
        groups = (checker.IR_SHARED_TYPE, checker.PENDING_BUS_MIGRATION,
                  checker.PRIVATE_HEADER_REACH, checker.CORE_LINKED_REACH,
                  checker.FLAT_ROOT_REACH)
        union: set = set()
        for group in groups:
            self.assertEqual(union & group, set())
            union |= group
        self.assertEqual(union, checker.ALLOWED)

    def test_private_reaches_are_recorded_under_the_private_root(self) -> None:
        for path, header in checker.PRIVATE_HEADER_REACH:
            with self.subTest(path=path):
                self.assertTrue(header.startswith("modules/"), f"{path} -> {header}")
        for path, header in checker.IR_SHARED_TYPE | checker.PENDING_BUS_MIGRATION:
            with self.subTest(path=path):
                self.assertTrue(header.startswith("aimee/"), f"{path} -> {header}")


if __name__ == "__main__":
    unittest.main()
