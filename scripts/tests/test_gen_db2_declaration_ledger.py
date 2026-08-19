#!/usr/bin/env python3
"""Failure-mode tests for the DB2 declaration ledger."""

from __future__ import annotations

import copy
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
SCRIPT = REPO_ROOT / "scripts/gen_db2_declaration_ledger.py"
SPEC = importlib.util.spec_from_file_location("gen_db2_declaration_ledger", SCRIPT)
assert SPEC and SPEC.loader
ledger = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = ledger
SPEC.loader.exec_module(ledger)


class DeclarationLedgerTests(unittest.TestCase):
    def fixture(self) -> tempfile.TemporaryDirectory[str]:
        temporary = tempfile.TemporaryDirectory()
        root = Path(temporary.name)
        shutil.copytree(REPO_ROOT / ledger.BOUNDARY, root / ledger.BOUNDARY)
        for relative in (ledger.SOURCE_BASELINE, ledger.REVIEW, ledger.OUTPUT):
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO_ROOT / relative, target)
        baseline = json.loads((REPO_ROOT / ledger.SOURCE_BASELINE).read_text(encoding="utf-8"))
        for row in baseline["consumers"]:
            source = REPO_ROOT / row["path"]
            target = root / row["path"]
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        return temporary

    def test_production_ledger_is_reproducible_and_exhaustive(self) -> None:
        ledger.run(REPO_ROOT, False)
        value = ledger.build(REPO_ROOT)
        self.assertEqual(value["summary"], {
            "headers": 138,
            "declarations": 1397,
            "reviewed": 339,
            "audit_pending": 619,
            "internal_unconsumed": 153,
            "private_test_only": 286,
        })
        self.assertFalse(value["declarations_complete"])
        self.assertEqual(
            [row["symbol"] for row in value["declarations"]],
            sorted({row["symbol"] for row in value["declarations"]}),
        )
        pgvector = [row for row in value["declarations"] if row["symbol"].startswith("pgvec_")
                    and row["status"] == "reviewed"]
        self.assertEqual(len(pgvector), 61)
        self.assertTrue(all(row["review"]["disposition"] == "private-db2" and
                            row["review"]["db3_placement"] == "retained-db2"
                            for row in pgvector))
        health = next(row for row in value["declarations"]
                      if row["symbol"] == "db2_health_probe")
        self.assertEqual(health["review"]["disposition"], "wire-operation")
        self.assertEqual(health["review"]["db3_placement"], "retained-db2")

    def test_parser_handles_linkage_multiline_callbacks_and_comments(self) -> None:
        source = r'''
#ifndef SAMPLE_H
#define SAMPLE_H
extern "C" {
/* hidden(fake()); */
int db2_one(
    const char *value,
    int (*callback)(void *context, int value));
const char *db2_two(void); // ignored(fake());
void *(db2_three)(void);
typedef int (*db2_callback_t)(int);
static int db2_private(void);
}
#endif
'''
        rows = ledger.declarations_from_text(source, "sample.h")
        self.assertEqual([row.symbol for row in rows], ["db2_one", "db2_two", "db2_three"])
        self.assertEqual([row.line for row in rows], [6, 9, 10])
        self.assertNotIn("hidden", rows[0].signature)

    def test_parser_ignores_structs_typedefs_inline_bodies_and_variables(self) -> None:
        source = '''
typedef struct sample { int (*callback)(int); } sample_t;
typedef int (*callback_t)(int);
int callback_slot;
static inline int helper(void) { return 1; }
int db2_public(void);
'''
        rows = ledger.declarations_from_text(source, "sample.h")
        self.assertEqual([row.symbol for row in rows], ["db2_public"])

    def test_identical_duplicates_are_recorded_and_conflicts_fail(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            boundary = root / ledger.BOUNDARY
            boundary.mkdir(parents=True)
            (boundary / "a.h").write_text("int db2_same(int value);\n", encoding="utf-8")
            (boundary / "b.h").write_text("int db2_same(int value);\n", encoding="utf-8")
            rows = ledger.extract_declarations(root)
            self.assertEqual(len(rows["db2_same"]["locations"]), 2)
            (boundary / "b.h").write_text("int db2_same(long value);\n", encoding="utf-8")
            with self.assertRaisesRegex(ledger.LedgerError, "rule=conflicting-declaration"):
                ledger.extract_declarations(root)

    def test_malformed_or_ambiguous_c_fails_closed(self) -> None:
        cases = (
            ("/*", "c-comment"),
            ('int x = "unterminated;', "c-literal"),
            ("int db2_x(void));", "c-parenthesis"),
            ("int db2_x(void) {", "c-brace"),
            ("extern int (*slot)(int);", "unsupported-extern"),
            ('extern "Rust" { int db2_x(void); }', "c-linkage"),
            ("foo(a) bar(b);", "ambiguous-declaration"),
            ("#define X \\", "preprocessor-continuation"),
            ("int db2_x(\x00);", "c-nul"),
        )
        for source, rule in cases:
            with self.subTest(rule=rule), self.assertRaisesRegex(
                    ledger.LedgerError, rf"rule={rule}"):
                ledger.declarations_from_text(source, "bad.h")

    def test_token_resource_limit_is_enforced(self) -> None:
        with mock.patch.object(ledger, "MAX_TOKENS", 3):
            with self.assertRaisesRegex(ledger.LedgerError, "rule=c-token-limit"):
                ledger.tokenize("; ; ; ;", "large.h")

    def test_consumer_tokenization_keeps_macro_references(self) -> None:
        source = "#define CALL_DB2() db2_from_macro()\n"
        self.assertNotIn("db2_from_macro", {
            token.text for token in ledger.tokenize(source, "consumer.c")
        })
        self.assertIn("db2_from_macro", {
            token.text for token in ledger.tokenize(
                source, "consumer.c", drop_directives=False)
        })

    def test_review_transitions_reject_stale_or_unsafe_rows(self) -> None:
        declarations = {"pgvec_search": {
            "signature": "int pgvec_search ( void )",
            "signature_sha256": "a" * 64,
            "locations": [],
        }}
        base = {
            "schema_version": 1,
            "module": "db2",
            "declarations_complete": False,
            "reviews": [{
                "symbol": "pgvec_search",
                "signature_sha256": "a" * 64,
                "disposition": "private-db2",
                "family": "index",
                "db3_placement": "retained-db2",
                "reason": "provider-specific implementation",
            }],
        }
        ledger._review_rows(base, declarations)
        cases = (
            (lambda value: value["reviews"][0].__setitem__("signature_sha256", "b" * 64),
             "review-signature"),
            (lambda value: value["reviews"][0].__setitem__("db3_placement", "db3-eligible"),
             "pgvector-placement"),
            (lambda value: value["reviews"][0].__setitem__("family", "unknown"),
             "review-value"),
            (lambda value: value["reviews"][0].__setitem__("extra", "x"),
             "review-shape"),
        )
        for mutate, rule in cases:
            value = copy.deepcopy(base)
            mutate(value)
            with self.subTest(rule=rule), self.assertRaisesRegex(
                    ledger.LedgerError, rf"rule={rule}"):
                ledger._review_rows(value, declarations)

        duplicate = copy.deepcopy(base)
        duplicate["reviews"].append(copy.deepcopy(duplicate["reviews"][0]))
        with self.assertRaisesRegex(ledger.LedgerError, "rule=review-order"):
            ledger._review_rows(duplicate, declarations)

    def test_header_and_consumer_symlinks_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            boundary = root / ledger.BOUNDARY
            boundary.mkdir(parents=True)
            outside = root / "outside.h"
            outside.write_text("int db2_outside(void);\n", encoding="utf-8")
            (boundary / "linked.h").symlink_to(outside)
            with self.assertRaisesRegex(ledger.LedgerError, "rule=header-symlink"):
                ledger.extract_declarations(root)

        temporary = self.fixture()
        try:
            root = Path(temporary.name)
            baseline = json.loads((root / ledger.SOURCE_BASELINE).read_text(encoding="utf-8"))
            path = root / baseline["consumers"][0]["path"]
            path.unlink()
            path.symlink_to(root / ledger.REVIEW)
            declarations = ledger.extract_declarations(root)
            with self.assertRaisesRegex(ledger.LedgerError, "rule=consumer-path"):
                ledger.consumer_index(root, declarations)
        finally:
            temporary.cleanup()

    def test_production_and_test_only_consumers_are_distinguished(self) -> None:
        value = ledger.build(REPO_ROOT)
        classes = {row["path"]: row["classification"] for row in value["consumer_classes"]}
        pending = next(row for row in value["declarations"]
                       if row["status"] == "audit-pending")
        test_only = next(row for row in value["declarations"]
                         if row["status"] == "private-test-only")
        self.assertTrue(any(classes[path] != "private-implementation-test"
                            for path in pending["consumers"]))
        self.assertTrue(all(classes[path] == "private-implementation-test"
                            for path in test_only["consumers"]))

    def test_write_check_drift_and_failed_write_preserves_output(self) -> None:
        temporary = self.fixture()
        try:
            root = Path(temporary.name)
            ledger.run(root, False)
            output = root / ledger.OUTPUT
            original = output.read_bytes()
            output.write_text("drift\n", encoding="utf-8")
            with self.assertRaisesRegex(ledger.LedgerError, "rule=generated-drift"):
                ledger.run(root, False)
            output.write_bytes(original)
            review_path = root / ledger.REVIEW
            review = json.loads(review_path.read_text(encoding="utf-8"))
            review["reviews"][0]["signature_sha256"] = "0" * 64
            review_path.write_text(json.dumps(review), encoding="utf-8")
            with self.assertRaisesRegex(ledger.LedgerError, "rule=review-signature"):
                ledger.run(root, True)
            self.assertEqual(output.read_bytes(), original)

            shutil.copy2(REPO_ROOT / ledger.REVIEW, review_path)
            review = json.loads(review_path.read_text(encoding="utf-8"))
            review["declarations_complete"] = True
            review_path.write_text(json.dumps(review), encoding="utf-8")
            with self.assertRaisesRegex(ledger.LedgerError, "rule=premature-completeness"):
                ledger.run(root, True)
            self.assertEqual(output.read_bytes(), original)
        finally:
            temporary.cleanup()

    def test_output_symlink_is_rejected(self) -> None:
        temporary = self.fixture()
        try:
            root = Path(temporary.name)
            output = root / ledger.OUTPUT
            output.unlink()
            output.symlink_to(root / ledger.REVIEW)
            with self.assertRaisesRegex(ledger.LedgerError, "rule=output-symlink"):
                ledger.run(root, False)
            with self.assertRaisesRegex(ledger.LedgerError, "rule=output-symlink"):
                ledger.run(root, True)
        finally:
            temporary.cleanup()

    def test_strict_json_failures(self) -> None:
        cases = (
            (b'{"x":1,"x":2}', "json-duplicate-key"),
            (b"\xef\xbb\xbf{}", "json-bom"),
            (b"\xff", "json-parse"),
            (b"{", "json-parse"),
        )
        for raw, rule in cases:
            with tempfile.TemporaryDirectory() as tmp, self.subTest(rule=rule):
                path = Path(tmp) / "value.json"
                path.write_bytes(raw)
                with self.assertRaisesRegex(ledger.LedgerError, rf"rule={rule}"):
                    ledger.load_json(path)

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            target = root / "target.json"
            target.write_text("{}", encoding="utf-8")
            link = root / "link.json"
            link.symlink_to(target)
            with self.assertRaisesRegex(ledger.LedgerError, "rule=json-symlink"):
                ledger.load_json(link)

    def test_cli_is_cwd_independent(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run(
                [sys.executable, "-I", "-S", str(SCRIPT)],
                cwd=tmp,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("gen_db2_declaration_ledger: ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
