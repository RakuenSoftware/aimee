#!/usr/bin/env python3
"""Tests for the descriptor-v1 envelope and taxonomy convergence."""

from __future__ import annotations

import copy
import errno
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
CHECKER = REPO_ROOT / "scripts/validate_module_descriptors.py"
SPEC = importlib.util.spec_from_file_location("validate_module_descriptors", CHECKER)
assert SPEC and SPEC.loader
validator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(validator)


def production_repo() -> tempfile.TemporaryDirectory[str]:
    tmp = tempfile.TemporaryDirectory()
    repo = Path(tmp.name)
    inventory = repo / validator.INVENTORY_PATH
    inventory.parent.mkdir(parents=True)
    shutil.copy2(REPO_ROOT / validator.INVENTORY_PATH, inventory)
    for source in (REPO_ROOT / "src/modules").glob("*/module.yaml"):
        target = repo / "src/modules" / source.parent.name / "module.yaml"
        target.parent.mkdir(parents=True)
        shutil.copy2(source, target)
        descriptor = json.loads(source.read_text(encoding="utf-8"))
        for field in validator.OWNERSHIP_FIELDS:
            for relative in descriptor.get(field, []):
                owned_target = repo / relative
                owned_target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(REPO_ROOT / relative, owned_target)
    return tmp


class DescriptorTests(unittest.TestCase):
    def required(self) -> dict[str, object]:
        return {
            "descriptor_version": 1,
            "id": "memory",
            "dependencies": ["config", "ir"],
            "runtime_toggle": {"supported": False},
        }

    def optional(self) -> dict[str, object]:
        return {
            "descriptor_version": 1,
            "id": "runtime-web",
            "dependencies": ["config", "gateway"],
            "enabled_by_default": True,
            "runtime_toggle": {"supported": True},
        }

    def taxonomy(self) -> tuple[set[str], set[str]]:
        return validator.load_inventory(REPO_ROOT)

    def production_repo(self) -> tempfile.TemporaryDirectory[str]:
        return production_repo()

    def mutate_descriptor(self, repo: Path, identifier: str, mutate) -> None:
        path = repo / "src/modules" / identifier / "module.yaml"
        value = json.loads(path.read_text(encoding="utf-8"))
        mutate(value)
        path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")

    def assert_rule(self, value: object, rule: str) -> None:
        required, optional = self.taxonomy()
        with self.assertRaisesRegex(validator.DescriptorError, f"rule={rule}"):
            validator.validate_descriptor(value, required, optional)

    def write_raw(self, raw: bytes) -> tuple[tempfile.TemporaryDirectory[str], Path]:
        tmp = tempfile.TemporaryDirectory()
        path = Path(tmp.name) / "value.json"
        path.write_bytes(raw)
        return tmp, path

    def test_positive_fixture_root_and_exact_count(self) -> None:
        count = validator.validate_roots(
            REPO_ROOT, [Path("tests/fixtures/modules/positive")]
        )
        self.assertEqual(count, 3)

    def test_complete_production_graph(self) -> None:
        required, optional = self.taxonomy()
        self.assertEqual(len(required | optional), 27)
        self.assertEqual(validator.validate_roots(REPO_ROOT, [Path("src/modules")]), 27)

    def test_schema_is_generated_byte_for_byte(self) -> None:
        validator.check_schema(REPO_ROOT)
        parsed = json.loads(validator.schema_bytes())
        self.assertEqual(parsed, validator.schema())
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            path = repo / validator.SCHEMA_PATH
            path.parent.mkdir(parents=True)
            path.write_text("{}\n", encoding="utf-8")
            with self.assertRaisesRegex(validator.DescriptorError, "rule=schema-drift"):
                validator.check_schema(repo)

    def test_schema_keyword_subset_is_closed_and_exercised(self) -> None:
        allowed = {
            "$schema", "$id", "$comment", "type", "additionalProperties", "required",
            "properties", "const", "pattern", "items", "uniqueItems",
        }

        def keys(value: object) -> set[str]:
            if isinstance(value, dict):
                found = set(value) - set(value.get("properties", {}))
                for key, item in value.items():
                    if key == "properties":
                        for subschema in item.values():
                            found |= keys(subschema)
                        continue
                    found |= keys(item)
                return found
            if isinstance(value, list):
                found: set[str] = set()
                for item in value:
                    found |= keys(item)
                return found
            return set()

        self.assertEqual(keys(validator.schema()), allowed)

    def test_inventory_is_the_only_taxonomy(self) -> None:
        required, optional = self.taxonomy()
        raw = json.loads((REPO_ROOT / validator.INVENTORY_PATH).read_text())
        self.assertEqual(required, set(raw["required"]))
        self.assertEqual(optional, set(raw["optional"]))
        self.assertFalse(required & optional)
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            path = repo / validator.INVENTORY_PATH
            path.parent.mkdir(parents=True)
            path.write_text(
                json.dumps({"schema_version": True, "required": ["memory"], "optional": ["x"]}),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(validator.DescriptorError, "rule=inventory-version"):
                validator.load_inventory(repo)

    def test_descriptor_key_and_version_mutations(self) -> None:
        missing = self.required()
        del missing["dependencies"]
        self.assert_rule(missing, "descriptor-keys")
        extra = self.required()
        extra["routes"] = []
        self.assert_rule(extra, "descriptor-keys")
        version = self.required()
        version["descriptor_version"] = 2
        self.assert_rule(version, "descriptor-version")

    def test_module_identity_mutations(self) -> None:
        for value in ("Memory", "memory_", "memory/child", "a" * 65):
            descriptor = self.required()
            descriptor["id"] = value
            with self.subTest(value=value):
                self.assert_rule(descriptor, "module-id")
        unknown = self.required()
        unknown["id"] = "unknown-module"
        self.assert_rule(unknown, "module-unknown")

    def test_dependency_mutations(self) -> None:
        cases = (
            (["ir", "config"], "dependency-order"),
            (["config", "config"], "dependency-duplicate"),
            (["memory"], "dependency-self"),
            (["unknown-module"], "dependency-unknown"),
        )
        for dependencies, rule in cases:
            descriptor = self.required()
            descriptor["dependencies"] = dependencies
            with self.subTest(rule=rule):
                self.assert_rule(descriptor, rule)

    def test_selection_and_runtime_toggle_mutations(self) -> None:
        required_default = self.required()
        required_default["enabled_by_default"] = True
        self.assert_rule(required_default, "descriptor-keys")
        required_toggle = self.required()
        required_toggle["runtime_toggle"]["supported"] = True
        self.assert_rule(required_toggle, "required-runtime-toggle")
        optional_missing = self.optional()
        del optional_missing["enabled_by_default"]
        self.assert_rule(optional_missing, "descriptor-keys")
        optional_type = self.optional()
        optional_type["enabled_by_default"] = None
        self.assert_rule(optional_type, "default-type")
        runtime_empty = self.required()
        runtime_empty["runtime_toggle"] = {}
        self.assert_rule(runtime_empty, "runtime-toggle-shape")
        complete_type = self.required()
        complete_type["ownership_complete"] = 1
        self.assert_rule(complete_type, "ownership-complete-type")

    def test_ownership_is_optional_and_report_preserves_declared_order(self) -> None:
        report = validator.ownership_report(REPO_ROOT, [Path("src/modules")])
        self.assertEqual(report["schema_version"], 1)
        self.assertEqual(report["result"], "PASS")
        descriptors = report["descriptors"]
        self.assertEqual([item["id"] for item in descriptors], sorted(item["id"] for item in descriptors))
        runtime = next(item for item in descriptors if item["id"] == "module-runtime")
        self.assertEqual(runtime["module_root"], "src/modules/module-runtime")
        self.assertEqual(
            [item["path"] for item in runtime["ownership"]["sources"]],
            [
                "src/modules/module-runtime/pre_llm_hook.c",
            ],
        )
        self.assertEqual(
            runtime["ownership"]["docs"],
            [{"path": "docs/modules/module-runtime.md", "result": "PASS"}],
        )
        # Every production descriptor is now ownership-latched. A new empty descriptor is
        # migration debt and must make this assertion fail.
        undeclared = [
            item for item in descriptors
            if not any(item["ownership"][field] for field in validator.OWNERSHIP_FIELDS)
        ]
        self.assertEqual(undeclared, [])

    def test_ownership_report_sorts_descriptors_independent_of_root_order(self) -> None:
        root = Path("tests/fixtures/modules/positive")
        roots = [root / "runtime-web", root / "memory", root / "control-web"]
        forward = validator.ownership_report(REPO_ROOT, roots)
        reverse = validator.ownership_report(REPO_ROOT, list(reversed(roots)))
        self.assertEqual(forward, reverse)
        self.assertEqual(
            [item["id"] for item in forward["descriptors"]],
            ["control-web", "memory", "runtime-web"],
        )

    def test_each_ownership_role_is_validated(self) -> None:
        descriptor = json.loads(
            (REPO_ROOT / "src/modules/module-runtime/module.yaml").read_text(encoding="utf-8")
        )
        report = validator.validate_ownership(REPO_ROOT, "module-runtime", descriptor)
        self.assertEqual(
            {field: len(report["ownership"][field]) for field in validator.OWNERSHIP_FIELDS},
            {"sources": 1, "private_headers": 0, "public_headers": 1, "tests": 1,
             "docs": 1, "go_sources": 0, "go_tests": 0},
        )

    def test_production_complete_ownership_mutations(self) -> None:
        cases = (
            ("roundtable", "sources", "src/modules/roundtable/roundtable_verify.c"),
            ("roundtable", "private_headers", "src/modules/roundtable/roundtable_verify.h"),
            ("protocols", "sources", "src/modules/protocols/acp/acp_server.c"),
            ("protocols", "private_headers",
             "src/modules/protocols/mcp/mcp_tools_gateway.h"),
            ("ir", "sources", "src/modules/ir/aimee_ir.c"),
            ("translation", "sources",
             "src/modules/translation/aimee_frontend_anthropic.c"),
            ("translation", "sources",
             "src/modules/translation/aimee_backend_openai.c"),
            ("translation", "sources", "src/modules/translation/aimee_ir_stream.c"),
            ("skills", "sources", "src/modules/skills/skill.c"),
            ("skills", "sources", "src/modules/skills/skill_rollback.c"),
            ("skills", "sources", "src/modules/skills/skill_trigger_policy.c"),
            ("skills", "private_headers", "src/modules/skills/skill_trigger_policy.h"),
            ("audit", "sources", "src/modules/audit/audit_action.c"),
            ("audit", "sources", "src/modules/audit/audit_worm.c"),
            ("audit", "sources", "src/modules/audit/audit_worm_chain.c"),
            ("module-runtime", "sources", "src/modules/module-runtime/pre_llm_hook.c"),
            ("benchmarks", "sources", "src/modules/benchmarks/agent_eval.c"),
            ("benchmarks", "private_headers", "src/modules/benchmarks/agent_eval.h"),
            ("tools", "sources", "src/modules/tools/agent_tools_dispatch.c"),
            ("tools", "go_sources", "server-go/modules/tools/tools.go"),
            ("tools", "go_tests", "server-go/modules/tools/tools_test.go"),
            ("routing", "sources", "src/modules/routing/routing.c"),
            ("routing", "go_sources", "server-go/modules/routing/routing.go"),
            ("routing", "go_tests", "server-go/modules/routing/routing_test.go"),
            ("execution-policy", "sources", "src/modules/execution-policy/execution_policy.c"),
            ("kb-synthesis", "sources", "src/modules/kb-synthesis/kb_curator_synthesize.c"),
            ("kb-synthesis", "private_headers", "src/modules/kb-synthesis/kb_curator_synthesize.h"),
            ("gateway", "sources", "src/modules/gateway/gateway_delegate.c"),
            ("gateway", "sources", "src/modules/gateway/gateway_pipeline.c"),
            ("gateway", "sources", "src/modules/gateway/gateway_policy.c"),
            ("governance", "sources", "src/modules/governance/gw_stage_governance.c"),
            ("governance", "private_headers", "src/modules/governance/gw_stage_governance.h"),
            ("learning", "sources", "src/modules/learning/learning_router.c"),
            ("learning", "go_sources", "server-go/modules/learning/learning.go"),
            ("learning", "go_tests", "server-go/modules/learning/learning_test.go"),
            ("skills", "go_sources", "server-go/modules/skills/skills.go"),
            ("skills", "go_tests", "server-go/modules/skills/skills_test.go"),
            ("workspace", "sources", "src/modules/workspace/workspace_turn.c"),
            ("workspace", "private_headers", "src/modules/workspace/workspace_provider.h"),
            ("workspace", "go_sources", "server-go/modules/workspace/workspace.go"),
            ("workspace", "go_tests", "server-go/modules/workspace/workspace_test.go"),
            ("vault", "sources", "src/modules/vault/vault_service.c"),
            ("vault", "private_headers", "src/modules/vault/vault_internal.h"),
            ("config", "sources", "src/modules/config/config.c"),
            ("config", "private_headers", "src/modules/config/config_internal.h"),
            ("git", "sources", "src/modules/git/git_ops.c"),
            ("git", "private_headers", "src/modules/git/git_verify_internal.h"),
            ("git", "go_sources", "server-go/modules/git/git.go"),
            ("git", "go_tests", "server-go/modules/git/git_test.go"),
            ("delegates", "sources", "src/modules/delegates/delegate_driver.c"),
            ("delegates", "go_sources", "server-go/modules/delegates/delegates.go"),
            ("delegates", "go_tests", "server-go/modules/delegates/delegates_test.go"),
            ("response-composition", "go_sources",
             "server-go/modules/response-composition/response_composition.go"),
            ("response-composition", "go_tests",
             "server-go/modules/response-composition/response_composition_test.go"),
            ("workflows", "sources", "src/modules/workflows/wfe_engine.c"),
            ("workflows", "private_headers", "src/modules/workflows/wfe_engine.h"),
            ("memory", "sources", "src/modules/memory/memory_core.c"),
            ("memory", "private_headers", "src/modules/memory/memory_core_internal.h"),
            ("memory", "go_sources", "server-go/modules/memory/memory.go"),
            ("memory", "go_tests", "server-go/modules/memory/memory_test.go"),
            ("roundtable", "sources", "src/modules/roundtable/module_adapter.c"),
            ("roundtable", "go_sources", "server-go/modules/roundtable/roundtable.go"),
            ("roundtable", "go_tests", "server-go/modules/roundtable/roundtable_test.go"),
            ("benchmarks", "sources", "src/modules/benchmarks/module_adapter.c"),
            ("benchmarks", "go_sources", "server-go/modules/benchmarks/benchmarks.go"),
            ("benchmarks", "go_tests", "server-go/modules/benchmarks/benchmarks_test.go"),
            ("governance", "sources", "src/modules/governance/module_adapter.c"),
            ("governance", "go_sources", "server-go/modules/governance/governance.go"),
            ("governance", "go_tests", "server-go/modules/governance/governance_test.go"),
            ("workflows", "sources", "src/modules/workflows/module_adapter.c"),
            ("workflows", "go_sources", "server-go/modules/workflows/workflows.go"),
            ("workflows", "go_tests", "server-go/modules/workflows/workflows_test.go"),
            ("kb-synthesis", "sources", "src/modules/kb-synthesis/module_adapter.c"),
            ("kb-synthesis", "go_sources", "server-go/modules/kb-synthesis/kb_synthesis.go"),
            ("kb-synthesis", "go_tests",
             "server-go/modules/kb-synthesis/kb_synthesis_test.go"),
            ("runtime-web", "sources", "src/modules/runtime-web/module_adapter.c"),
            ("runtime-web", "go_sources",
             "server-go/modules/runtime-web/policy/status.go"),
            ("runtime-web", "go_sources",
             "server-go/modules/runtime-web/runtime_web.go"),
            ("runtime-web", "go_tests",
             "server-go/modules/runtime-web/runtime_web_test.go"),
            ("control-web", "sources", "src/modules/control-web/module_adapter.c"),
            ("control-web", "go_sources",
             "server-go/modules/control-web/policy/acl.go"),
            ("control-web", "go_sources",
             "server-go/modules/control-web/control_web.go"),
            ("control-web", "go_tests",
             "server-go/modules/control-web/control_web_test.go"),
        )
        for identifier, field, relative in cases:
            tmp = self.production_repo()
            try:
                repo = Path(tmp.name)
                self.mutate_descriptor(
                    repo, identifier,
                    lambda value, field=field, relative=relative:
                        value[field].remove(relative),
                )
                with self.subTest(identifier=identifier, field=field), self.assertRaisesRegex(
                    validator.DescriptorError,
                    rf"rule=ownership-complete pointer=/{field}.*missing=.*{Path(relative).name}",
                ):
                    validator.validate_roots(repo, [Path("src/modules")])
            finally:
                tmp.cleanup()

        for identifier in ("benchmarks", "tools", "routing", "execution-policy", "kb-synthesis", "runtime-web", "control-web", "roundtable", "protocols", "ir", "translation", "skills",
                           "audit", "module-runtime", "gateway", "governance",
                           "learning", "workspace", "vault", "config", "git", "delegates",
                           "workflows", "memory"):
            for name in ("undeclared.c", "undeclared.h"):
                tmp = self.production_repo()
                try:
                    repo = Path(tmp.name)
                    (repo / "src/modules" / identifier / name).write_text(
                        "/* planted */\n", encoding="utf-8"
                    )
                    role = "sources" if name.endswith(".c") else "private_headers"
                    with self.subTest(identifier=identifier, name=name), \
                            self.assertRaisesRegex(
                                validator.DescriptorError,
                                rf"rule=ownership-complete pointer=/{role}.*missing=.*{name}",
                            ):
                        validator.validate_roots(repo, [Path("src/modules")])
                finally:
                    tmp.cleanup()

    def test_latched_descriptors_declare_complete_ownership(self) -> None:
        """The latch itself is the control; mutation coverage below assumes it stays set.

        Derived from the graph rather than a hardcoded list, so a newly latched module is
        covered without editing this test and the set cannot drift behind the descriptors.
        """
        latched = 0
        for path in sorted((REPO_ROOT / "src/modules").glob("*/module.yaml")):
            descriptor = json.loads(path.read_text(encoding="utf-8"))
            if descriptor.get("ownership_complete") is None:
                continue
            latched += 1
            with self.subTest(identifier=path.parent.name):
                self.assertIs(descriptor.get("ownership_complete"), True)
        # Guard only against a vacuous pass (broken glob, every descriptor unlatched); the
        # per-descriptor assertion above does the real work. A count floor is deliberately
        # avoided — it would drift on every latch, the exact churn the graph scan removes.
        self.assertTrue(latched, "no latched descriptor found; the guard would pass vacuously")

    def test_empty_module_root_cannot_be_latched(self) -> None:
        """An unmigrated module must not satisfy the latch vacuously."""
        tmp = self.production_repo()
        try:
            repo = Path(tmp.name)
            shutil.rmtree(repo / "src/modules/control-web")
            shutil.rmtree(repo / "server-go/modules/control-web")
            (repo / "docs/modules/control-web.md").unlink()
            root = repo / "src/modules/control-web"
            root.mkdir(parents=True)
            (root / "module.yaml").write_text(json.dumps({
                "descriptor_version": 1,
                "id": "control-web",
                "dependencies": ["config", "gateway", "module-runtime", "protocols"],
                "enabled_by_default": True,
                "runtime_toggle": {"supported": True},
                "ownership_complete": True,
                "docs": ["docs/modules/control-web.md"],
            }) + "\n", encoding="utf-8")
            document = repo / "docs/modules/control-web.md"
            document.parent.mkdir(parents=True, exist_ok=True)
            document.write_text("placeholder\n", encoding="utf-8")
            with self.assertRaisesRegex(
                validator.DescriptorError,
                r"rule=ownership-empty-domain pointer=/ownership_complete",
            ):
                validator.validate_roots(repo, [Path("src/modules")])
        finally:
            tmp.cleanup()

    def test_a_single_module_local_file_is_enough_domain_to_latch(self) -> None:
        """The guard rejects an empty domain, not a small one."""
        tmp = self.production_repo()
        try:
            repo = Path(tmp.name)
            shutil.rmtree(repo / "src/modules/control-web")
            shutil.rmtree(repo / "server-go/modules/control-web")
            (repo / "docs/modules/control-web.md").unlink()
            source = repo / "src/modules/control-web/control_web_stub.c"
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text("/* planted */\n", encoding="utf-8")
            document = "docs/modules/control-web.md"
            (source.parent / "module.yaml").write_text(json.dumps({
                "descriptor_version": 1,
                "id": "control-web",
                "dependencies": ["config", "gateway", "module-runtime", "protocols"],
                "enabled_by_default": True,
                "runtime_toggle": {"supported": True},
                "ownership_complete": True,
                "docs": [document],
                "sources": ["src/modules/control-web/control_web_stub.c"],
            }) + "\n", encoding="utf-8")
            (repo / document).parent.mkdir(parents=True, exist_ok=True)
            (repo / document).write_text("placeholder\n", encoding="utf-8")
            validator.validate_roots(repo, [Path("src/modules")])
        finally:
            tmp.cleanup()

    def test_latched_modules_all_have_a_non_empty_domain(self) -> None:
        """Derived from the graph, so latching a tenth module needs no edit here."""
        latched = 0
        for path in sorted((REPO_ROOT / "src/modules").glob("*/module.yaml")):
            descriptor = json.loads(path.read_text(encoding="utf-8"))
            if descriptor.get("ownership_complete") is not True:
                continue
            latched += 1
            with self.subTest(identifier=path.parent.name):
                self.assertTrue(descriptor.get("sources") or descriptor.get("private_headers"))
        self.assertTrue(latched, "no latched descriptor found; the guard would be untested")

    def test_complete_ownership_requires_canonical_doc(self) -> None:
        for identifier in ("benchmarks", "tools", "routing", "execution-policy", "kb-synthesis", "runtime-web", "control-web", "roundtable", "ir", "translation", "skills", "audit",
                           "module-runtime", "gateway", "governance", "learning",
                           "workspace", "vault", "config", "git", "delegates", "workflows",
                           "memory"):
            tmp = self.production_repo()
            try:
                repo = Path(tmp.name)
                self.mutate_descriptor(repo, identifier,
                                       lambda value: value.__setitem__("docs", []))
                with self.subTest(identifier=identifier), self.assertRaisesRegex(
                    validator.DescriptorError, r"rule=ownership-complete pointer=/docs"
                ):
                    validator.validate_roots(repo, [Path("src/modules")])
            finally:
                tmp.cleanup()

    def test_complete_ownership_excludes_undeclared_public_headers(self) -> None:
        tmp = self.production_repo()
        try:
            repo = Path(tmp.name)
            public = repo / "src/modules/roundtable/include/aimee/roundtable/public.h"
            public.parent.mkdir(parents=True, exist_ok=True)
            public.write_text("/* public contract */\n", encoding="utf-8")
            self.assertEqual(
                validator.validate_roots(repo, [Path("src/modules")]), 27
            )
        finally:
            tmp.cleanup()

    def test_complete_ownership_rejects_undeclared_symlink(self) -> None:
        tmp = self.production_repo()
        try:
            repo = Path(tmp.name)
            link = repo / "src/modules/roundtable/alias.h"
            link.symlink_to(repo / "src/modules/roundtable/roundtable_types.h")
            with self.assertRaisesRegex(
                validator.DescriptorError,
                r"rule=ownership-complete-symlink pointer=/private_headers.*alias.h",
            ):
                validator.validate_roots(repo, [Path("src/modules")])
        finally:
            tmp.cleanup()

    def test_complete_ownership_symlink_variants_fail_stably(self) -> None:
        cases = (
            ("declared.h", "roundtable_types.h", True, "ownership-path-symlink"),
            ("broken.h", "missing.h", False, "ownership-complete-symlink"),
            ("directory.h", ".", False, "ownership-complete-symlink"),
        )
        for name, target, declared, rule in cases:
            tmp = self.production_repo()
            try:
                repo = Path(tmp.name)
                relative = f"src/modules/roundtable/{name}"
                (repo / relative).symlink_to(target)
                if declared:
                    self.mutate_descriptor(
                        repo,
                        "roundtable",
                        lambda value, relative=relative:
                            value["private_headers"].append(relative),
                    )
                with self.subTest(name=name), self.assertRaisesRegex(
                    validator.DescriptorError,
                    rf"rule={rule} pointer=/private_headers",
                ):
                    validator.validate_roots(repo, [Path("src/modules")])
            finally:
                tmp.cleanup()

    def test_public_header_cannot_be_declared_private(self) -> None:
        descriptor = json.loads(
            (REPO_ROOT / "src/modules/module-runtime/module.yaml").read_text(encoding="utf-8")
        )
        descriptor["private_headers"] = [descriptor["public_headers"][0]]
        with self.assertRaisesRegex(
            validator.DescriptorError,
            r"rule=ownership-role-boundary pointer=/private_headers/0",
        ):
            validator.validate_ownership(REPO_ROOT, "module-runtime", descriptor)

    def test_ownership_duplicate_and_cross_role_mutations(self) -> None:
        descriptor = json.loads(
            (REPO_ROOT / "src/modules/module-runtime/module.yaml").read_text(encoding="utf-8")
        )
        duplicate = copy.deepcopy(descriptor)
        duplicate["sources"].append(duplicate["sources"][0])
        with self.assertRaisesRegex(validator.DescriptorError, "rule=ownership-duplicate"):
            validator.validate_ownership(REPO_ROOT, "module-runtime", duplicate)

        cross_role = copy.deepcopy(descriptor)
        cross_role["tests"] = [cross_role["sources"][0]]
        with self.assertRaisesRegex(validator.DescriptorError, "rule=ownership-cross-role"):
            validator.validate_ownership(REPO_ROOT, "module-runtime", cross_role)

    def test_cross_descriptor_duplicate_includes_prior_location(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            memory = self.required()
            memory["tests"] = ["src/tests/test_plugin_c_hook.c"]
            config = {
                "descriptor_version": 1,
                "id": "config",
                "dependencies": ["module-runtime"],
                "runtime_toggle": {"supported": False},
                "tests": ["src/tests/test_plugin_c_hook.c"],
            }
            for name, descriptor in (("one", memory), ("two", config)):
                path = root / name / "module.yaml"
                path.parent.mkdir()
                path.write_text(json.dumps(descriptor), encoding="utf-8")
            with self.assertRaisesRegex(
                validator.DescriptorError,
                r"rule=ownership-cross-descriptor.*memory tests at /tests/0",
            ):
                validator.validate_roots(REPO_ROOT, [root])

    def test_role_policy_must_cover_every_ownership_field(self) -> None:
        descriptor = self.required()
        original = validator.ROLE_EXTENSIONS.pop("sources")
        try:
            with self.assertRaisesRegex(validator.DescriptorError, "rule=ownership-role-undefined"):
                validator.validate_ownership(REPO_ROOT, "memory", descriptor)
        finally:
            validator.ROLE_EXTENSIONS["sources"] = original

    def test_ownership_path_and_role_mutations(self) -> None:
        descriptor = json.loads(
            (REPO_ROOT / "src/modules/module-runtime/module.yaml").read_text(encoding="utf-8")
        )
        cases = (
            ("sources", "/tmp/extension.c", "ownership-path-normalized"),
            ("sources", "src/modules/module-runtime/../config/config.c", "ownership-path-normalized"),
            ("sources", "src/modules/config/config.c", "ownership-role-boundary"),
            ("sources", "src/modules/module-runtime/module.yaml", "ownership-role"),
            ("public_headers", "src/modules/module-runtime/module.yaml", "ownership-role-boundary"),
            ("tests", "src/tests/Rules.mk", "ownership-role"),
            ("docs", "docs/modules/config.md", "ownership-doc-canonical"),
            ("docs", "docs/modules/module-runtime.txt", "ownership-doc-canonical"),
            ("sources", "src/modules/module-runtime/missing.c", "ownership-file"),
        )
        for field, path, rule in cases:
            mutated = copy.deepcopy(descriptor)
            mutated[field] = [path]
            with self.subTest(field=field, path=path), self.assertRaisesRegex(
                validator.DescriptorError, f"rule={rule}"
            ):
                validator.validate_ownership(REPO_ROOT, "module-runtime", mutated)

        for value, rule in (("not-an-array", "ownership-field-type"), ([7], "ownership-path-type")):
            mutated = copy.deepcopy(descriptor)
            mutated["sources"] = value
            with self.subTest(value=value), self.assertRaisesRegex(
                validator.DescriptorError, f"rule={rule}"
            ):
                validator.validate_ownership(REPO_ROOT, "module-runtime", mutated)

    def test_ownership_nonregular_and_symlink_escape_mutations(self) -> None:
        tmp = self.production_repo()
        try:
            repo = Path(tmp.name)
            directory = repo / "src/modules/module-runtime/not-a-file.c"
            directory.mkdir()
            self.mutate_descriptor(
                repo, "module-runtime", lambda value: value.__setitem__(
                    "sources", ["src/modules/module-runtime/not-a-file.c"]
                )
            )
            with self.assertRaisesRegex(validator.DescriptorError, "rule=ownership-file"):
                validator.validate_roots(repo, [Path("src/modules")])
        finally:
            tmp.cleanup()

        tmp = self.production_repo()
        outside = Path(tmp.name).parent / f"{Path(tmp.name).name}-outside.c"
        try:
            repo = Path(tmp.name)
            outside.write_text("outside\n", encoding="utf-8")
            link = repo / "src/modules/module-runtime/escape.c"
            link.symlink_to(outside)
            self.mutate_descriptor(
                repo, "module-runtime", lambda value: value.__setitem__(
                    "sources", ["src/modules/module-runtime/escape.c"]
                )
            )
            with self.assertRaisesRegex(validator.DescriptorError, "rule=ownership-path-escape"):
                validator.validate_roots(repo, [Path("src/modules")])
        finally:
            outside.unlink(missing_ok=True)
            tmp.cleanup()

    def test_inward_and_self_referential_symlinks_fail_stably(self) -> None:
        tmp = self.production_repo()
        try:
            repo = Path(tmp.name)
            link = repo / "src/modules/module-runtime/alias.c"
            link.symlink_to(repo / "src/modules/module-runtime/extension.c")
            self.mutate_descriptor(
                repo, "module-runtime", lambda value: value.__setitem__(
                    "sources", ["src/modules/module-runtime/alias.c"]
                )
            )
            with self.assertRaisesRegex(
                validator.DescriptorError, r"rule=ownership-path-symlink pointer=/sources/0"
            ):
                validator.validate_roots(repo, [Path("src/modules")])
        finally:
            tmp.cleanup()

    def test_resolve_errors_are_classified_without_masking_io(self) -> None:
        path = Path("owned.c")
        with mock.patch.object(Path, "resolve", side_effect=OSError(errno.ELOOP, "loop")):
            with self.assertRaisesRegex(validator.DescriptorError, "rule=ownership-file"):
                validator._resolve_owned(path, "/sources/0")
        with mock.patch.object(
            Path, "resolve", side_effect=OSError(errno.ENAMETOOLONG, "long")
        ):
            with self.assertRaisesRegex(
                validator.DescriptorError, "rule=ownership-path-normalized"
            ):
                validator._resolve_owned(path, "/sources/0")
        with mock.patch.object(Path, "resolve", side_effect=OSError(errno.EACCES, "denied")):
            with self.assertRaises(OSError):
                validator._resolve_owned(path, "/sources/0")

        tmp = self.production_repo()
        try:
            repo = Path(tmp.name)
            link = repo / "src/modules/module-runtime/loop.c"
            link.symlink_to(link)
            self.mutate_descriptor(
                repo, "module-runtime", lambda value: value.__setitem__(
                    "sources", ["src/modules/module-runtime/loop.c"]
                )
            )
            with self.assertRaisesRegex(
                validator.DescriptorError, r"rule=ownership-file pointer=/sources/0"
            ):
                validator.validate_roots(repo, [Path("src/modules")])
        finally:
            tmp.cleanup()


    def test_duplicate_module_id_fails_across_roots(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ("one", "two"):
                path = root / name / "module.yaml"
                path.parent.mkdir()
                path.write_text(json.dumps(self.required()), encoding="utf-8")
            with self.assertRaisesRegex(validator.DescriptorError, "rule=module-duplicate"):
                validator.validate_roots(REPO_ROOT, [root])

    def test_empty_root_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaisesRegex(validator.DescriptorError, "rule=no-descriptors-found"):
                validator.validate_roots(REPO_ROOT, [root])

    def test_production_coverage_and_path_mutations(self) -> None:
        tmp = self.production_repo()
        try:
            repo = Path(tmp.name)
            (repo / "src/modules/config/module.yaml").unlink()
            with self.assertRaisesRegex(validator.DescriptorError, "rule=production-coverage"):
                validator.validate_roots(repo, [Path("src/modules")])
        finally:
            tmp.cleanup()

        tmp = self.production_repo()
        try:
            repo = Path(tmp.name)
            source = repo / "src/modules/config/module.yaml"
            target = repo / "src/modules/Config/module.yaml"
            target.parent.mkdir()
            source.rename(target)
            with self.assertRaisesRegex(validator.DescriptorError, "rule=production-path"):
                validator.validate_roots(repo, [Path("src/modules")])
        finally:
            tmp.cleanup()

    def test_production_selection_policy_mutations(self) -> None:
        required, optional = self.taxonomy()
        for identifier in sorted(optional):
            tmp = self.production_repo()
            try:
                repo = Path(tmp.name)
                self.mutate_descriptor(
                    repo, identifier,
                    lambda value: value.__setitem__(
                        "enabled_by_default", not value["enabled_by_default"]
                    ),
                )
                with self.subTest(identifier=identifier, field="enabled_by_default"), \
                        self.assertRaisesRegex(validator.DescriptorError, "rule=production-default"):
                    validator.validate_roots(repo, [Path("src/modules")])
            finally:
                tmp.cleanup()

        for identifier in sorted(required | optional):
            tmp = self.production_repo()
            try:
                repo = Path(tmp.name)
                self.mutate_descriptor(
                    repo, identifier,
                    lambda value: value["runtime_toggle"].__setitem__(
                        "supported", not value["runtime_toggle"]["supported"]
                    ),
                )
                rule = (
                    "required-runtime-toggle"
                    if identifier in required
                    else "production-runtime-toggle"
                )
                with self.subTest(identifier=identifier, field="runtime_toggle"), \
                        self.assertRaisesRegex(validator.DescriptorError, f"rule={rule}"):
                    validator.validate_roots(repo, [Path("src/modules")])
            finally:
                tmp.cleanup()

    def test_production_graph_mutations(self) -> None:
        tmp = self.production_repo()
        try:
            repo = Path(tmp.name)
            self.mutate_descriptor(
                repo, "config", lambda value: value.__setitem__(
                    "dependencies", ["module-runtime", "runtime-web"]
                )
            )
            with self.assertRaisesRegex(validator.DescriptorError, "rule=core-to-optional"):
                validator.validate_roots(repo, [Path("src/modules")])
        finally:
            tmp.cleanup()

        tmp = self.production_repo()
        try:
            repo = Path(tmp.name)
            self.mutate_descriptor(
                repo, "module-runtime",
                lambda value: value.__setitem__("dependencies", ["config"]),
            )
            with self.assertRaisesRegex(
                validator.DescriptorError,
                r"rule=dependency-cycle.*config -> module-runtime -> config",
            ):
                validator.validate_roots(repo, [Path("src/modules")])
        finally:
            tmp.cleanup()

    def test_aliased_production_root_cannot_bypass_policy(self) -> None:
        tmp = self.production_repo()
        try:
            repo = Path(tmp.name)
            self.mutate_descriptor(
                repo, "runtime-web",
                lambda value: value.__setitem__("enabled_by_default", False),
            )
            with self.assertRaisesRegex(validator.DescriptorError, "rule=production-default"):
                validator.validate_roots(repo, [Path("src/modules/../modules")])
        finally:
            tmp.cleanup()

    def test_inventory_partition_swap_fails_closed(self) -> None:
        tmp = self.production_repo()
        try:
            repo = Path(tmp.name)
            path = repo / validator.INVENTORY_PATH
            value = json.loads(path.read_text(encoding="utf-8"))
            value["required"].remove("config")
            value["required"].append("governance")
            value["optional"].remove("governance")
            value["optional"].append("config")
            path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
            with self.assertRaisesRegex(
                validator.DescriptorError, r"config/module.yaml.*rule=descriptor-keys"
            ):
                validator.validate_roots(repo, [Path("src/modules")])
        finally:
            tmp.cleanup()

    def test_parser_resource_limits(self) -> None:
        tmp, path = self.write_raw(b" " * (validator.MAX_BYTES + 1))
        try:
            with self.assertRaisesRegex(validator.DescriptorError, "rule=input-size"):
                validator.load_json(path)
        finally:
            tmp.cleanup()
        nested: object = True
        for _ in range(validator.MAX_DEPTH + 1):
            nested = [nested]
        with self.assertRaisesRegex(validator.DescriptorError, "rule=json-depth"):
            validator._check_domain(nested)
        with self.assertRaisesRegex(validator.DescriptorError, "rule=json-array-size"):
            validator._check_domain([None] * (validator.MAX_ARRAY + 1))

    def test_strict_json_rejections(self) -> None:
        cases = (
            (b"{\"a\":1,\"a\":2}", "json-duplicate-key"),
            (b"{\"a\":{\"b\":1,\"b\":2}}", "json-duplicate-key"),
            (b"\xef\xbb\xbf{}", "json-bom"),
            (b"{# comment\n}", "json-parse"),
            (b"{a: 1}", "json-parse"),
            (b"{\"a\": .NaN}", "json-parse"),
            (b"{\"a\": NaN}", "json-number-domain"),
            (b"{\"a\": 1.0}", "json-number-domain"),
            (b"{\"a\": 1e2}", "json-number-domain"),
            (b"{} trailing", "json-parse"),
            (b"---\na: on\n", "json-parse"),
        )
        for raw, rule in cases:
            tmp, path = self.write_raw(raw)
            try:
                with self.subTest(raw=raw), self.assertRaisesRegex(
                    validator.DescriptorError, f"rule={rule}"
                ):
                    validator.load_json(path)
            finally:
                tmp.cleanup()

    def test_crlf_is_valid_and_surrogate_is_rejected(self) -> None:
        tmp, path = self.write_raw(b'{\r\n  "value": true\r\n}\r\n')
        try:
            self.assertEqual(validator.load_json(path), {"value": True})
        finally:
            tmp.cleanup()
        tmp, path = self.write_raw(b'{"value":"\\ud800"}')
        try:
            with self.assertRaisesRegex(validator.DescriptorError, "json-surrogate"):
                validator.load_json(path)
        finally:
            tmp.cleanup()


class OwnershipCliTests(unittest.TestCase):
    def run_checker(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, "-I", "-S", str(CHECKER), *args],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    def test_emit_ownership_is_one_complete_json_report(self) -> None:
        result = self.run_checker("--check-schema", "--emit-ownership", "src/modules")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        payload = json.loads(result.stdout)
        self.assertEqual(payload["schema_version"], 1)
        self.assertEqual(payload["result"], "PASS")
        self.assertEqual(len(payload["descriptors"]), 27)
        for descriptor in payload["descriptors"]:
            self.assertEqual(
                set(descriptor), {"id", "module_root", "ownership", "result"}
            )

    def test_emit_flags_are_mutually_exclusive(self) -> None:
        result = self.run_checker(
            "--emit-schema", "--emit-ownership", "src/modules"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertIn("rule=cli-flag-conflict pointer=/args", result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_emit_ownership_failure_has_stable_rule_without_json(self) -> None:
        tmp = production_repo()
        try:
            repo = Path(tmp.name)
            descriptor_path = repo / "src/modules/module-runtime/module.yaml"
            descriptor = json.loads(descriptor_path.read_text(encoding="utf-8"))
            descriptor["sources"] = ["src/modules/module-runtime/loop.c"]
            descriptor_path.write_text(json.dumps(descriptor), encoding="utf-8")
            link = repo / "src/modules/module-runtime/loop.c"
            link.symlink_to(link)
            result = self.run_checker(
                "--config-root", str(repo), "--emit-ownership", "src/modules"
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "")
            self.assertIn("rule=ownership-file pointer=/sources/0", result.stderr)
            self.assertNotIn("Traceback", result.stderr)
        finally:
            tmp.cleanup()


if __name__ == "__main__":
    unittest.main()
