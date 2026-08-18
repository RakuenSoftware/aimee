#!/usr/bin/env python3
"""Failure-mode and reproducibility tests for the DB2 catalog generator."""

from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
GENERATOR = REPO_ROOT / "scripts/gen_db2_contract.py"
SPEC = importlib.util.spec_from_file_location("gen_db2_contract", GENERATOR)
assert SPEC and SPEC.loader
generator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(generator)


def _first_memory_index(catalog: dict[str, object]) -> int:
    """Position of the first memory operation, so family-boundary mutations stay
    anchored as later operations are appended to the catalog."""
    return next(index for index, operation in enumerate(catalog["operations"])
                if operation["family"] == "memory")


class ContractTests(unittest.TestCase):
    def catalog(self) -> dict[str, object]:
        return json.loads((REPO_ROOT / generator.CATALOG).read_text(encoding="utf-8"))

    def assert_rule(self, mutate, rule: str) -> None:
        value = copy.deepcopy(self.catalog())
        mutate(value)
        with self.assertRaisesRegex(generator.ContractError, rf"rule={rule}"):
            generator.validate_catalog(value)

    def fixture(self) -> tempfile.TemporaryDirectory[str]:
        temporary = tempfile.TemporaryDirectory()
        root = Path(temporary.name)
        for relative in (
            generator.CATALOG,
            generator.DESCRIPTOR,
            generator.PROCESS_CONTRACTS,
            generator.HEADER,
            generator.CLIENT_HEADER,
            generator.CLIENT_SOURCE,
            generator.GO_CONTRACT,
            generator.BASELINE,
            generator.DECLARATION_REVIEW,
            generator.DECLARATION_LEDGER,
        ):
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO_ROOT / relative, target)
        return temporary

    def test_production_catalog_and_generated_outputs_match(self) -> None:
        generator.run(REPO_ROOT, False)
        catalog = generator.validate_catalog(generator.load_json(REPO_ROOT / generator.CATALOG))
        header, client_header, client_source, go_contract, baseline = generator.generated(REPO_ROOT)
        self.assertEqual(header, (REPO_ROOT / generator.HEADER).read_bytes())
        self.assertEqual(client_header, (REPO_ROOT / generator.CLIENT_HEADER).read_bytes())
        self.assertEqual(client_source, (REPO_ROOT / generator.CLIENT_SOURCE).read_bytes())
        self.assertEqual(go_contract, (REPO_ROOT / generator.GO_CONTRACT).read_bytes())
        self.assertEqual(baseline, (REPO_ROOT / generator.BASELINE).read_bytes())
        fingerprint = generator.catalog_fingerprint(catalog)
        self.assertIn(fingerprint.encode(), header)
        self.assertIn(b"#define AIMEE_DB2_RESULT_INVALID_STATE 5u", header)
        self.assertIn(b"aimee_db2_request_header_decode", header)
        self.assertIn(b"AIMEE_DB2_ENVELOPE_HEADER_LEN", header)
        self.assertIn(b"aimee_db2_health_call", client_header)
        self.assertIn(b"aimee_db2_embedding_dimension_call", client_header)
        self.assertIn(b"aimee_db2_pool_status_call", client_header)
        self.assertIn(b"aimee_db2_embedding_refusals_call", client_header)
        self.assertIn(b"aimee_db2_postgres_status_call", client_header)
        self.assertIn(b"aimee_db2_reembed_status_call", client_header)
        self.assertIn(b"aimee_db2_reembed_clear_call", client_header)
        self.assertIn(b"AIMEE_MODULE_CALL_PROTOCOL", client_source)
        self.assertIn(fingerprint.encode(), go_contract)
        self.assertIn(b"func DecodeHealthResponse", go_contract)
        self.assertIn(b"func DecodeRequestHeader", go_contract)
        self.assertIn(b"func DecodeReplyHeader", go_contract)
        self.assertIn(b"func DecodeEmbeddingDimensionReply", go_contract)
        self.assertIn(b"func DecodePoolStatusReply", go_contract)
        self.assertIn(b"func DecodeEmbeddingRefusalsReply", go_contract)
        self.assertIn(b"func DecodePostgresStatusReply", go_contract)
        self.assertIn(b"func DecodeReembedStatusReply", go_contract)
        self.assertIn(b"func DecodeReembedClearReply", go_contract)
        self.assertIn(b"ErrMalformedHealth", go_contract)
        self.assertIn(b"ResultOK", go_contract)
        self.assertIn(b"HealthFlagPGTrgm", go_contract)
        self.assertIn(b"HealthFlagKBTables", go_contract)
        self.assertEqual(json.loads(baseline)["catalog_sha256"], fingerprint)
        self.assertEqual(
            json.loads(baseline)["result_codes"],
            [{"id": index, "name": name} for index, name in enumerate(generator.RESULT_CODES)],
        )

    def test_additive_body_envelope_vectors_are_closed_and_fixed_width(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        envelope = baseline["body_envelope"]
        self.assertEqual(envelope["header_len"], generator.ENVELOPE_HEADER_LEN)
        request = bytes.fromhex(envelope["request"]["positive"])
        self.assertEqual(len(request), generator.ENVELOPE_HEADER_LEN + 3)
        self.assertEqual(int.from_bytes(request[0:4], "little"),
                         generator.ENVELOPE_REQUEST_MAGIC)
        self.assertEqual(int.from_bytes(request[6:8], "little"),
                         generator.ENVELOPE_HEADER_LEN)
        self.assertEqual(int.from_bytes(request[16:20], "little"), 3)
        self.assertEqual(
            [row["mutation"] for row in envelope["request"]["negative"]],
            ["bad_magic", "bad_version", "bad_header_len", "zero_operation",
             "payload_length", "reserved", "short", "long"],
        )
        self.assertEqual(
            [row["result"] for row in envelope["reply"]["positive"]],
            list(range(len(generator.RESULT_CODES))),
        )
        self.assertEqual(
            [row["mutation"] for row in envelope["reply"]["negative"]],
            ["bad_magic", "bad_version", "bad_header_len", "zero_operation",
             "unknown_result", "payload_length", "reserved", "short", "long"],
        )

    def test_wire_vectors_cover_every_flag_and_closed_failure_fields(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][0]
        self.assertEqual([row["flags"] for row in operation["reply"]["positive"]], list(range(8)))
        self.assertEqual(
            [row["mutation"] for row in operation["request"]["negative"]],
            ["bad_magic", "bad_version", "short", "long"],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["bad_magic", "bad_version", "unknown_flags", "reserved", "short", "long"],
        )
        self.assertTrue(all(len(bytes.fromhex(row["hex"])) == 16
                            for row in operation["reply"]["positive"]))

    def test_embedding_dimension_vectors_cover_results_and_bounds(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][1]
        self.assertEqual(operation["name"], "embedding_dimension")
        self.assertEqual(
            [(row["result"], row["dimension"])
             for row in operation["reply"]["positive"]],
            [(0, 384), (5, 0)],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["request"]["negative"]],
            ["bad_flags", "payload_length", "short", "long"],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["wrong_operation", "unsupported_result", "ok_without_payload",
             "error_with_payload", "zero_dimension", "dimension_too_large",
             "short", "long"],
        )

    def test_level3_count_vectors_cover_closed_result_and_bound(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][10]
        self.assertEqual(operation["name"], "level3_count")
        self.assertEqual(
            [(row["result"], row["count"]) for row in operation["reply"]["positive"]],
            [(0, 42)],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["wrong_operation", "unsupported_result", "ok_without_payload",
             "count_too_large", "short", "long"],
        )

    def test_level2_count_vectors_cover_closed_result_and_bound(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][11]
        self.assertEqual(operation["name"], "level2_count")
        self.assertEqual(
            [(row["result"], row["count"]) for row in operation["reply"]["positive"]],
            [(0, 17)],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["wrong_operation", "unsupported_result", "ok_without_payload",
             "count_too_large", "short", "long"],
        )

    def test_orphaned_l0_count_vectors_cover_closed_result_and_bound(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][12]
        self.assertEqual(operation["name"], "orphaned_l0_count")
        self.assertEqual(
            [(row["result"], row["count"]) for row in operation["reply"]["positive"]],
            [(0, 5)],
        )

    def test_total_count_vectors_cover_closed_result_and_bound(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][13]
        self.assertEqual(operation["name"], "total_count")
        self.assertEqual(
            [(row["result"], row["count"]) for row in operation["reply"]["positive"]],
            [(0, 1234567890123)],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["wrong_operation", "unsupported_result", "ok_without_payload",
             "count_too_large", "short", "long"],
        )

    def test_session_l2_count_vectors_cover_string_and_count_bounds(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][14]
        self.assertEqual(operation["name"], "session_l2_count")
        self.assertEqual(operation["request"]["source_session"], "session-123")
        self.assertEqual(
            [row["mutation"] for row in operation["request"]["negative"]],
            ["bad_flags", "empty_session", "length_mismatch", "session_too_large",
             "embedded_nul", "short", "long"],
        )
        self.assertEqual(
            [(row["result"], row["count"]) for row in operation["reply"]["positive"]],
            [(0, 3)],
        )

    def test_key_exists_vectors_cover_key_and_boolean_bounds(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][15]
        self.assertEqual(operation["name"], "key_exists")
        self.assertEqual(operation["request"]["key"], "recovery:tool-a->tool-b")
        self.assertEqual(
            [row["mutation"] for row in operation["request"]["negative"]],
            ["bad_flags", "empty_key", "length_mismatch", "key_too_large",
             "embedded_nul", "short", "long"],
        )
        self.assertEqual(
            [(row["result"], row["exists"]) for row in operation["reply"]["positive"]],
            [(0, 1)],
        )

    def test_find_id_by_key_kind_vectors_cover_strings_and_result_consistency(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][16]
        self.assertEqual(operation["name"], "find_id_by_key_kind")
        self.assertEqual(operation["request"]["key"], "task:deploy-fix")
        self.assertEqual(operation["request"]["kind"], "task")
        self.assertEqual(
            [row["mutation"] for row in operation["request"]["negative"]],
            ["bad_flags", "empty_key", "key_length_mismatch", "key_too_large",
             "key_embedded_nul", "empty_kind", "kind_length_mismatch", "kind_too_large",
             "kind_embedded_nul", "short", "long"],
        )
        self.assertEqual(
            [(row["result"], row["found"], row["id"])
             for row in operation["reply"]["positive"]],
            [(0, 1, 42)],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["wrong_operation", "unsupported_result", "ok_without_payload",
             "found_too_large", "absent_with_id", "present_without_id", "id_too_large",
             "short", "long"],
        )

    def test_key_exists_in_tier_pair_vectors_cover_three_strings_and_boolean(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][17]
        self.assertEqual(operation["name"], "key_exists_in_tier_pair")
        self.assertEqual(operation["request"]["key"], "recovery:tool-a->tool-b")
        self.assertEqual(operation["request"]["tier_a"], "L3")
        self.assertEqual(operation["request"]["tier_b"], "L4")
        self.assertEqual(
            [row["mutation"] for row in operation["request"]["negative"]],
            ["bad_flags", "empty_key", "key_length_mismatch", "key_too_large",
             "key_embedded_nul", "empty_tier_a", "tier_a_length_mismatch",
             "tier_a_too_large", "tier_a_embedded_nul", "empty_tier_b",
             "tier_b_length_mismatch", "tier_b_too_large", "tier_b_embedded_nul",
             "short", "long"],
        )
        self.assertEqual(
            [(row["result"], row["exists"]) for row in operation["reply"]["positive"]],
            [(0, 1)],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["wrong_operation", "unsupported_result", "ok_without_payload",
             "exists_too_large", "short", "long"],
        )

    def test_effectiveness_update_vectors_preserve_binary64_and_closed_results(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][18]
        self.assertEqual(operation["name"], "effectiveness_update")
        self.assertEqual(
            (operation["request"]["memory_id"], operation["request"]["has_value"],
             operation["request"]["value_bits"]),
            (42, 1, 0x3fe8000000000000),
        )
        self.assertEqual(
            [row["mutation"] for row in operation["request"]["negative"]],
            ["bad_flags", "zero_memory_id", "memory_id_too_large", "has_value_too_large",
             "clear_with_value", "short", "long"],
        )
        self.assertEqual(
            [row["result"] for row in operation["reply"]["positive"]],
            [0, 5],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["wrong_operation", "unsupported_result", "payload_length", "short", "long"],
        )

    def test_retention_enforce_vectors_cover_fixed_policy_result(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][19]
        self.assertEqual(operation["name"], "retention_enforce")
        self.assertEqual(
            [row["mutation"] for row in operation["request"]["negative"]],
            ["bad_flags", "payload_length", "short", "long"],
        )
        self.assertEqual(
            [(row["result"], row["deleted_count"])
             for row in operation["reply"]["positive"]],
            [(0, 4)],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["wrong_operation", "unsupported_result", "ok_without_payload",
             "deleted_count_too_large", "short", "long"],
        )

    def test_effectiveness_demote_vectors_cover_fixed_threshold_result(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][20]
        self.assertEqual(operation["name"], "effectiveness_demote")
        self.assertEqual(operation["request"]["threshold_bits"], 0x3fd3333333333333)
        self.assertEqual(
            [row["mutation"] for row in operation["request"]["negative"]],
            ["bad_flags", "payload_length", "short", "long"],
        )
        self.assertEqual(
            [(row["result"], row["demoted_count"])
             for row in operation["reply"]["positive"]],
            [(0, 2)],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["wrong_operation", "unsupported_result", "ok_without_payload",
             "demoted_count_too_large", "short", "long"],
        )

    def test_effectiveness_stats_vectors_cover_fixed_threshold_summary(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][21]
        self.assertEqual(operation["name"], "effectiveness_stats")
        self.assertEqual(operation["request"]["low_threshold_bits"], 0x3fd3333333333333)
        self.assertEqual(
            [row["mutation"] for row in operation["request"]["negative"]],
            ["bad_flags", "payload_length", "short", "long"],
        )
        self.assertEqual(
            [(row["result"], row["avg_effectiveness_bits"], row["low_effectiveness_count"],
              row["high_impact_count"])
             for row in operation["reply"]["positive"]],
            [(0, 0x3fe0000000000000, 3, 1)],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["wrong_operation", "unsupported_result", "ok_without_payload",
             "average_above_maximum", "average_negative", "average_not_a_number",
             "low_effectiveness_count_too_large", "high_impact_count_too_large",
             "short", "long"],
        )

    def test_l2_memory_ids_vectors_cover_bounded_identifier_list(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][22]
        self.assertEqual(operation["name"], "l2_memory_ids")
        self.assertEqual(operation["request"]["maximum_ids"], 2048)
        self.assertEqual(
            [row["mutation"] for row in operation["request"]["negative"]],
            ["bad_flags", "payload_length", "short", "long"],
        )
        self.assertEqual(
            [(row["result"], row["memory_ids"]) for row in operation["reply"]["positive"]],
            [(0, [7, 19, 9223372036854775807]), (0, [])],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["wrong_operation", "unsupported_result", "ok_without_count",
             "count_exceeds_payload", "count_below_payload", "count_above_maximum",
             "identifier_zero", "identifier_above_maximum", "short", "long"],
        )

    def test_health_record_vectors_cover_the_three_cycle_counters(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][23]
        self.assertEqual(operation["name"], "health_record")
        self.assertEqual(operation["request"]["conflict_window_days"], 1)
        self.assertEqual(
            [operation["request"][name]
             for name in ("promotions", "demotions", "expirations")],
            [4, 2, 9],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["request"]["negative"]],
            ["bad_flags", "payload_length", "promotions_too_large", "demotions_too_large",
             "expirations_too_large", "short", "long"],
        )
        self.assertEqual(
            [row["result"] for row in operation["reply"]["positive"]], [0],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["wrong_operation", "unsupported_result", "unexpected_payload", "short", "long"],
        )

    def test_health_retention_vectors_cover_both_halves(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][24]
        self.assertEqual(operation["name"], "health_retention")
        self.assertEqual(operation["request"]["snapshot_retention_days"], 90)
        self.assertEqual(operation["request"]["contradiction_retention_days"], 90)
        self.assertEqual(
            [row["mutation"] for row in operation["request"]["negative"]],
            ["bad_flags", "payload_length", "short", "long"],
        )
        self.assertEqual(
            [(row["result"], row["snapshots_deleted"], row["contradictions_deleted"])
             for row in operation["reply"]["positive"]],
            [(0, 11, 3)],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["wrong_operation", "unsupported_result", "ok_without_payload",
             "snapshots_deleted_too_large", "contradictions_deleted_too_large",
             "short", "long"],
        )

    def test_health_counters_vectors_cover_the_whole_aggregate(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][25]
        self.assertEqual(operation["name"], "health_counters")
        self.assertEqual(operation["request"]["promote_use_count"], 3)
        self.assertEqual(operation["request"]["promote_confidence_bits"], 0x3feccccccccccccd)
        self.assertEqual(
            [row["mutation"] for row in operation["request"]["negative"]],
            ["bad_flags", "payload_length", "short", "long"],
        )
        self.assertEqual(
            operation["reply"]["positive"][0]["counters"],
            {"cycles": 7, "total_contradictions": 13, "total_promotions": 5,
             "total_demotions": 2, "total_expirations": 4, "new_memories": 21,
             "l1_eligible": 9, "l2_total": 30, "l2_stale_30_days": 6},
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["wrong_operation", "unsupported_result", "ok_without_payload",
             "first_counter_too_large", "last_counter_too_large", "short", "long"],
        )

    def test_stats_counts_vectors_cover_every_labelled_bucket(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][26]
        self.assertEqual(operation["name"], "stats_counts")
        self.assertEqual(
            [row["mutation"] for row in operation["request"]["negative"]],
            ["bad_flags", "payload_length", "short", "long"],
        )
        positive = operation["reply"]["positive"][0]
        self.assertEqual(positive["tier_counts"], [3, 12, 30, 8, 2, 1])
        self.assertEqual(positive["kind_counts"], [14, 5, 6, 9, 4, 3, 2, 1, 7, 5])
        # The reply's own total must agree with the tier breakdown it ships with.
        self.assertEqual(positive["total"], sum(positive["tier_counts"]))
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["wrong_operation", "unsupported_result", "ok_without_payload",
             "first_tier_too_large", "last_kind_too_large", "conflicts_too_large",
             "short", "long"],
        )

    def test_stats_counts_labels_match_the_catalog(self) -> None:
        catalog = json.loads((REPO_ROOT / generator.CATALOG).read_text(encoding="utf-8"))
        fields = catalog["operations"][26]["reply"]["fields"]
        self.assertEqual(fields[0]["labels"], list(generator.MEMORY_TIERS))
        self.assertEqual(fields[1]["labels"], list(generator.MEMORY_KINDS))
        # KIND_COUNT in src/headers/aimee.h is the authority for the bucket count.
        header = (REPO_ROOT / "src/headers/aimee.h").read_text(encoding="utf-8")
        kind_count = int(re.search(r"#define KIND_COUNT\s+(\d+)", header).group(1))
        self.assertEqual(len(generator.MEMORY_KINDS), kind_count)
        for kind in generator.MEMORY_KINDS:
            self.assertIn(f'"{kind}"', header)

    def test_expire_vectors_cover_both_stages(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][27]
        self.assertEqual(operation["name"], "expire")
        self.assertEqual(operation["request"]["stale_l1_tier"], "L1")
        self.assertEqual(operation["request"]["maximum_kinds"], 16)
        self.assertEqual(
            [row["mutation"] for row in operation["request"]["negative"]],
            ["bad_flags", "payload_length", "short", "long"],
        )
        self.assertEqual(
            [(row["result"], row["level0_deleted"], row["stale_level1_deleted"])
             for row in operation["reply"]["positive"]],
            [(0, 9, 17)],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["wrong_operation", "unsupported_result", "ok_without_payload",
             "level0_deleted_too_large", "stale_level1_deleted_too_large", "short", "long"],
        )

    def test_demote_vectors_cover_the_cascade_invariant(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][28]
        self.assertEqual(operation["name"], "demote")
        self.assertEqual(operation["request"]["demote_tier"], "L2")
        self.assertEqual(operation["request"]["maximum_kinds"], 16)
        self.assertEqual(
            [row["mutation"] for row in operation["request"]["negative"]],
            ["bad_flags", "payload_length", "short", "long"],
        )
        self.assertEqual(
            [(row["result"], row["demoted_count"], row["cascaded_count"])
             for row in operation["reply"]["positive"]],
            [(0, 6, 2), (0, 0, 0)],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["wrong_operation", "unsupported_result", "ok_without_payload",
             "demoted_count_too_large", "cascaded_count_too_large",
             "cascade_without_demotion", "short", "long"],
        )

    def test_pool_status_vectors_cover_results_and_relations(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][2]
        self.assertEqual(operation["name"], "pool_status")
        self.assertEqual(
            [(row["result"], row["size"], row["in_use"])
             for row in operation["reply"]["positive"]],
            [(0, 16, 2), (5, 0, 0)],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["wrong_operation", "unsupported_result", "ok_without_payload",
             "error_with_payload", "zero_size", "size_too_large", "in_use_too_large",
             "short", "long"],
        )

    def test_embedding_refusal_vectors_cover_relational_failures(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][3]
        self.assertEqual(operation["name"], "embedding_refusals")
        self.assertEqual(
            [(row["result"], row["refused_count"], row["last_offered"])
             for row in operation["reply"]["positive"]],
            [(0, 7, 768), (5, 0, 0)],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["wrong_operation", "unsupported_result", "ok_without_payload",
             "error_with_payload", "count_without_dimension", "dimension_without_count",
             "offered_too_large", "short", "long"],
        )

    def test_postgres_status_vectors_cover_availability_failures(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][4]
        self.assertEqual(operation["name"], "postgres_status")
        self.assertEqual(
            [(row["result"], row["available"], row["active_connections"],
              row["max_connections"], row["is_replica"], row["replica_lag_bytes"])
             for row in operation["reply"]["positive"]],
            [(0, 15, 12, 100, 1, 1048576), (0, 3, 12, 100, 0, 0),
             (5, 0, 0, 0, 0, 0)],
        )
        self.assertIn(
            "lag_on_primary",
            [row["mutation"] for row in operation["reply"]["negative"]],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["wrong_operation", "unsupported_result", "ok_without_payload",
             "error_with_payload", "unknown_availability", "active_without_availability",
             "max_without_availability", "role_without_availability",
             "lag_without_availability", "lag_on_primary", "invalid_replica_role", "short",
             "long"],
        )

    def test_reembed_status_vectors_cover_result_domain(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][5]
        self.assertEqual(operation["name"], "reembed_status")
        self.assertEqual(
            [(row["result"], row["target_dimension"], row["started_epoch"])
             for row in operation["reply"]["positive"]],
            [(0, 384, 1700000000), (1, 0, 0), (5, 0, 0)],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["wrong_operation", "unsupported_result", "ok_without_payload",
             "error_with_payload", "zero_dimension", "dimension_too_large", "zero_epoch",
             "short", "long"],
        )

    def test_reembed_clear_vectors_are_zero_payload(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][6]
        self.assertEqual(operation["name"], "reembed_clear")
        self.assertEqual(
            [row["result"] for row in operation["reply"]["positive"]],
            [0, 5],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["wrong_operation", "unsupported_result", "ok_with_payload",
             "error_with_payload", "short", "long"],
        )

    def test_embedder_serving_id_vectors_cover_bounds(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][8]
        self.assertEqual(operation["name"], "embedder_serving_id")
        self.assertEqual(
            [(row["result"], len(row["serving_id"]))
             for row in operation["reply"]["positive"]],
            [(0, 27), (0, 0), (0, 159), (5, 0)],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["wrong_operation", "unsupported_result", "ok_without_payload",
             "error_with_payload", "length_mismatch", "length_too_large",
             "embedded_nul", "short", "long"],
        )

    def test_dimension_reset_vectors_cover_closed_results_and_bounds(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][9]
        self.assertEqual(operation["name"], "dimension_reset")
        self.assertEqual(
            [row["result"] for row in operation["reply"]["positive"]],
            [0, 2, 3, 5],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["request"]["negative"]],
            ["bad_flags", "payload_length", "target_zero", "target_too_large",
             "invalid_force", "invalid_dry_run", "short", "long"],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["wrong_operation", "unsupported_result", "ok_without_payload",
             "error_with_payload", "target_zero", "tables_too_large", "short", "long"],
        )

    def test_root_and_version_mutations(self) -> None:
        cases = (
            (lambda value: value.__setitem__("extra", 1), "keys"),
            (lambda value: value.__setitem__("schema_version", 2), "schema-version"),
            (lambda value: value.__setitem__("module", "db3"), "module"),
            (lambda value: value.__setitem__("wire_version", True), "wire-version"),
            (lambda value: value.__setitem__("catalog_complete", 1), "catalog-complete-type"),
            (lambda value: value.__setitem__("catalog_complete", True), "catalog-complete"),
            (lambda value: value["body_envelope"].__setitem__("request_magic", 1),
             "body-envelope"),
            (lambda value: value["body_envelope"].__setitem__("reply_magic", 1),
             "body-envelope"),
            (lambda value: value["body_envelope"].__setitem__("header_len", 23),
             "body-envelope"),
            (lambda value: value["body_envelope"].__setitem__("extra", 1), "keys"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_family_mutations(self) -> None:
        cases = (
            (lambda value: value["families"].pop(), "families"),
            (lambda value: value["families"][0].__setitem__("name", "tenancy"),
             "family-order"),
            (lambda value: value["families"][0].__setitem__("id", 2), "family-id"),
            (lambda value: value["families"][0].__setitem__("event_kind", 11522),
             "family-event-kind"),
            (lambda value: value["families"][1].__setitem__("active", True),
             "family-active"),
            (lambda value: value["families"][0].__setitem__("extra", 1), "keys"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_operation_identity_and_semantic_mutations(self) -> None:
        cases = (
            (lambda value: value.__setitem__("result_codes", ["ok"]), "result-codes"),
            (lambda value: value.__setitem__("operations", []), "operations"),
            (lambda value: value["operations"][0].__setitem__("family", "unknown"),
             "operation-family"),
            (lambda value: value["operations"][0].__setitem__("id", True), "integer"),
            (lambda value: value["operations"][0].__setitem__("name", "Health"),
             "operation-name"),
            (lambda value: value["operations"][0].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][0].__setitem__("scope", "tenant"),
             "operation-semantics"),
            (lambda value: value["operations"][0].__setitem__("results", ["retryable"]),
             "operation-results"),
            (lambda value: value["operations"][0].__setitem__("db3_placement", "eligible"),
             "db3-placement"),
            (lambda value: value["operations"][0].__setitem__("c_symbols", []),
             "operation-c-symbols"),
            (lambda value: value["operations"][0].__setitem__("extra", 1), "keys"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_operation_duplicates_and_order_are_rejected(self) -> None:
        self.assert_rule(
            lambda value: value["operations"].append(copy.deepcopy(value["operations"][0])),
            "operation-duplicate",
        )
        self.assert_rule(
            lambda value: value["operations"].insert(_first_memory_index(value), {
                **copy.deepcopy(value["operations"][0]),
                "id": 11,
                "name": "health_second",
                "c_symbols": ["db2_health_second"],
            }),
            "unsupported-operation",
        )

    def test_health_wire_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][0]["request"].__setitem__("magic", 1),
             "health-request"),
            (lambda value: value["operations"][0]["request"].__setitem__("encoded_size", 9),
             "health-request"),
            (lambda value: value["operations"][0]["reply"].__setitem__("magic", 1),
             "health-reply"),
            (lambda value: value["operations"][0]["reply"].__setitem__("encoded_size", 15),
             "health-reply"),
            (lambda value: value["operations"][0]["reply"]["flags"][0].__setitem__("bit", True),
             "integer"),
            (lambda value: value["operations"][0]["reply"]["flags"][0].__setitem__("name", "x"),
             "health-flags"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_embedding_dimension_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][1].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][1].__setitem__("results", ["ok"]),
             "operation-results"),
            (lambda value: value["operations"][1]["request"].__setitem__("payload", "u32"),
             "embedding-dimension-request"),
            (lambda value: value["operations"][1]["reply"].__setitem__("encoded_size_ok", 24),
             "embedding-dimension-reply"),
            (lambda value: value["operations"][1]["reply"]["field"].__setitem__(
                "maximum", 4001), "embedding-dimension-reply"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_level3_count_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][10].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][10].__setitem__("results", ["ok", "invalid_state"]),
             "operation-results"),
            (lambda value: value["operations"][10]["request"].__setitem__("payload", "u32"),
             "level3-count-request"),
            (lambda value: value["operations"][10]["reply"]["field"].__setitem__(
                "maximum", 0xffffffff), "level3-count-reply"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_level2_count_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][11].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][11].__setitem__("results", ["ok", "invalid_state"]),
             "operation-results"),
            (lambda value: value["operations"][11]["request"].__setitem__("payload", "u32"),
             "level2-count-request"),
            (lambda value: value["operations"][11]["reply"]["field"].__setitem__(
                "maximum", 0xffffffff), "level2-count-reply"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_orphaned_l0_count_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][12].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][12].__setitem__("results", ["ok", "invalid_state"]),
             "operation-results"),
            (lambda value: value["operations"][12]["reply"]["field"].__setitem__(
                "maximum", 0xffffffff), "orphaned-l0-count-reply"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_total_count_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][13].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][13].__setitem__("results", ["ok", "invalid_state"]),
             "operation-results"),
            (lambda value: value["operations"][13]["request"].__setitem__("payload", "u64"),
             "total-count-request"),
            (lambda value: value["operations"][13]["reply"]["field"].__setitem__(
                "maximum", 0xffffffffffffffff), "total-count-reply"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_session_l2_count_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][14].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][14].__setitem__("results", ["ok", "invalid_state"]),
             "operation-results"),
            (lambda value: value["operations"][14]["request"]["field"].__setitem__(
                "maximum_bytes", 128), "session-l2-count-request"),
            (lambda value: value["operations"][14]["reply"]["field"].__setitem__(
                "maximum", 0xffffffff), "session-l2-count-reply"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_key_exists_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][15].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][15].__setitem__("results", ["ok", "invalid_state"]),
             "operation-results"),
            (lambda value: value["operations"][15]["request"]["field"].__setitem__(
                "maximum_bytes", 512), "key-exists-request"),
            (lambda value: value["operations"][15]["reply"]["field"].__setitem__(
                "maximum", 2), "key-exists-reply"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_find_id_by_key_kind_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][16].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][16].__setitem__(
                "results", ["ok", "not_found"]), "operation-results"),
            (lambda value: value["operations"][16]["request"]["fields"][0].__setitem__(
                "maximum_bytes", 512), "find-id-by-key-kind-request"),
            (lambda value: value["operations"][16]["request"]["fields"][1].__setitem__(
                "maximum_bytes", 16), "find-id-by-key-kind-request"),
            (lambda value: value["operations"][16]["reply"]["fields"][0].__setitem__(
                "maximum", 2), "find-id-by-key-kind-reply"),
            (lambda value: value["operations"][16]["reply"]["fields"][1].__setitem__(
                "maximum", 0xffffffffffffffff), "find-id-by-key-kind-reply"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_key_exists_in_tier_pair_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][17].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][17].__setitem__(
                "results", ["ok", "not_found"]), "operation-results"),
            (lambda value: value["operations"][17]["request"]["fields"][0].__setitem__(
                "maximum_bytes", 512), "key-exists-in-tier-pair-request"),
            (lambda value: value["operations"][17]["request"]["fields"][1].__setitem__(
                "maximum_bytes", 16), "key-exists-in-tier-pair-request"),
            (lambda value: value["operations"][17]["request"]["fields"][2].__setitem__(
                "maximum_bytes", 16), "key-exists-in-tier-pair-request"),
            (lambda value: value["operations"][17]["reply"]["field"].__setitem__(
                "maximum", 2), "key-exists-in-tier-pair-reply"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_effectiveness_update_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][18].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][18].__setitem__("results", ["ok"]),
             "operation-results"),
            (lambda value: value["operations"][18]["request"]["fields"][0].__setitem__(
                "minimum", 0), "effectiveness-update-request"),
            (lambda value: value["operations"][18]["request"]["fields"][1].__setitem__(
                "maximum", 2), "effectiveness-update-request"),
            (lambda value: value["operations"][18]["request"]["fields"][2].__setitem__(
                "encoding", "host-double"), "effectiveness-update-request"),
            (lambda value: value["operations"][18]["reply"].__setitem__(
                "encoded_size_ok", 28), "effectiveness-update-reply"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_retention_enforce_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][19].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][19].__setitem__("results", ["ok", "invalid_state"]),
             "operation-results"),
            (lambda value: value["operations"][19]["request"]["policy"][0].__setitem__(
                "retention_days", 8), "retention-enforce-request"),
            (lambda value: value["operations"][19]["request"]["policy"][1].__setitem__(
                "sensitivity", "secret"), "retention-enforce-request"),
            (lambda value: value["operations"][19]["reply"]["field"].__setitem__(
                "maximum", 0xffffffff), "retention-enforce-reply"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_effectiveness_demote_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][20].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][20].__setitem__("results", ["ok", "invalid_state"]),
             "operation-results"),
            (lambda value: value["operations"][20]["request"]["policy"].__setitem__(
                "threshold_binary64_bits", 0), "effectiveness-demote-request"),
            (lambda value: value["operations"][20]["reply"]["field"].__setitem__(
                "maximum", 0xffffffff), "effectiveness-demote-reply"),
            (lambda value: value["operations"][20].__setitem__("transaction", "none"),
             "operation-semantics"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_effectiveness_stats_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][21].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][21].__setitem__("results", ["ok", "invalid_state"]),
             "operation-results"),
            (lambda value: value["operations"][21]["request"]["policy"].__setitem__(
                "low_threshold_binary64_bits", 0), "effectiveness-stats-request"),
            (lambda value: value["operations"][21]["reply"]["fields"][0].__setitem__(
                "maximum_binary64_bits", 0x7ff8000000000000), "effectiveness-stats-reply"),
            (lambda value: value["operations"][21]["reply"]["fields"][1].__setitem__(
                "maximum", 0xffffffff), "effectiveness-stats-reply"),
            (lambda value: value["operations"][21]["reply"]["fields"][2].__setitem__(
                "maximum", 0xffffffff), "effectiveness-stats-reply"),
            (lambda value: value["operations"][21]["reply"]["fields"].pop(),
             "effectiveness-stats-reply"),
            (lambda value: value["operations"][21].__setitem__("transaction", "single-statement"),
             "operation-semantics"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_l2_memory_ids_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][22].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][22].__setitem__("results", ["ok", "not_found"]),
             "operation-results"),
            (lambda value: value["operations"][22]["request"]["policy"].__setitem__(
                "maximum_ids", 4096), "l2-memory-ids-request"),
            (lambda value: value["operations"][22]["reply"]["field"].__setitem__(
                "item_minimum", 0), "l2-memory-ids-reply"),
            (lambda value: value["operations"][22]["reply"]["field"].__setitem__(
                "maximum_items", 4096), "l2-memory-ids-reply"),
            (lambda value: value["operations"][22]["reply"].__setitem__(
                "encoded_size_max_ok", 16413), "l2-memory-ids-reply"),
            (lambda value: value["operations"][22].__setitem__("transaction", "single-statement"),
             "operation-semantics"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_health_record_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][23].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][23].__setitem__("results", ["ok", "conflict"]),
             "operation-results"),
            (lambda value: value["operations"][23]["request"]["policy"].__setitem__(
                "conflict_window_days", 7), "health-record-request"),
            (lambda value: value["operations"][23]["request"]["fields"][0].__setitem__(
                "maximum", 0xffffffff), "health-record-request"),
            (lambda value: value["operations"][23]["request"]["fields"].pop(),
             "health-record-request"),
            (lambda value: value["operations"][23]["reply"].__setitem__("encoded_size_ok", 28),
             "health-record-reply"),
            # A health-cycle insert is the one operation that is not replay-safe.
            (lambda value: value["operations"][23].__setitem__("idempotency", "safe"),
             "operation-semantics"),
            (lambda value: value["operations"][23].__setitem__("transaction", "none"),
             "operation-semantics"),
            # ...and no other operation may claim that exemption.
            (lambda value: value["operations"][22].__setitem__("idempotency", "unsafe"),
             "operation-semantics"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_health_retention_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][24].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][24].__setitem__("results", ["ok", "not_found"]),
             "operation-results"),
            (lambda value: value["operations"][24]["request"]["policy"].__setitem__(
                "snapshot_retention_days", 30), "health-retention-request"),
            (lambda value: value["operations"][24]["request"]["policy"].__setitem__(
                "contradiction_retention_days", 30), "health-retention-request"),
            (lambda value: value["operations"][24]["reply"]["fields"][1].__setitem__(
                "maximum", 0xffffffff), "health-retention-reply"),
            # Dropping a half would let one prune report as the whole action.
            (lambda value: value["operations"][24]["reply"]["fields"].pop(),
             "health-retention-reply"),
            (lambda value: value["operations"][24]["c_symbols"].pop(),
             "operation-c-symbols"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_health_counters_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][25].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][25].__setitem__("results", ["ok", "not_found"]),
             "operation-results"),
            (lambda value: value["operations"][25]["request"]["policy"].__setitem__(
                "promote_use_count", 5), "health-counters-request"),
            (lambda value: value["operations"][25]["request"]["policy"].__setitem__(
                "promote_confidence_binary64_bits", 0), "health-counters-request"),
            # Wire order is part of the contract, not just the field set.
            (lambda value: value["operations"][25]["reply"]["fields"].reverse(),
             "health-counters-reply"),
            (lambda value: value["operations"][25]["reply"]["fields"].pop(),
             "health-counters-reply"),
            (lambda value: value["operations"][25]["reply"]["fields"][8].__setitem__(
                "maximum", 0xffffffff), "health-counters-reply"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_stats_counts_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][26].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][26].__setitem__("results", ["ok", "not_found"]),
             "operation-results"),
            (lambda value: value["operations"][26]["request"].__setitem__("payload", "u32"),
             "stats-counts-request"),
            # Dropping the last kind is exactly the gap this operation closed.
            (lambda value: value["operations"][26]["reply"]["fields"][1]["labels"].pop(),
             "stats-counts-reply"),
            (lambda value: value["operations"][26]["reply"]["fields"][1].__setitem__("items", 9),
             "stats-counts-reply"),
            (lambda value: value["operations"][26]["reply"]["fields"][0]["labels"].reverse(),
             "stats-counts-reply"),
            (lambda value: value["operations"][26]["reply"].__setitem__("encoded_size_ok", 92),
             "stats-counts-reply"),
            (lambda value: value["operations"][26]["reply"]["fields"].pop(),
             "stats-counts-reply"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_expire_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][27].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][27].__setitem__("results", ["ok", "not_found"]),
             "operation-results"),
            (lambda value: value["operations"][27]["request"]["policy"].__setitem__(
                "stale_l1_tier", "L2"), "expire-request"),
            (lambda value: value["operations"][27]["request"]["policy"].__setitem__(
                "maximum_kinds", 64), "expire-request"),
            # Dropping a provenance delete would leave rows without their record.
            (lambda value: value["operations"][27]["c_symbols"].remove(
                "db2_memory_promotion_delete_stale_l1_provenance"), "operation-c-symbols"),
            (lambda value: value["operations"][27]["reply"]["fields"].pop(), "expire-reply"),
            (lambda value: value["operations"][27]["reply"]["fields"][1].__setitem__(
                "maximum", 0xffffffff), "expire-reply"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_demote_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][28].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][28].__setitem__("results", ["ok", "not_found"]),
             "operation-results"),
            (lambda value: value["operations"][28]["request"]["policy"].__setitem__(
                "demote_tier", "L1"), "demote-request"),
            (lambda value: value["operations"][28]["request"]["policy"].__setitem__(
                "maximum_kinds", 64), "demote-request"),
            # Dropping the cascade would let demoted rows keep confident dependants.
            (lambda value: value["operations"][28]["c_symbols"].remove(
                "db2_memory_promotion_demote_cascade"), "operation-c-symbols"),
            (lambda value: value["operations"][28]["reply"].__setitem__("consistency", ""),
             "demote-reply"),
            (lambda value: value["operations"][28]["reply"]["fields"][1].__setitem__(
                "maximum", 0xffffffff), "demote-reply"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_pool_status_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][2].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][2].__setitem__("results", ["ok"]),
             "operation-results"),
            (lambda value: value["operations"][2]["request"].__setitem__("payload", "u32"),
             "pool-status-request"),
            (lambda value: value["operations"][2]["reply"].__setitem__("encoded_size_ok", 64),
             "pool-status-reply"),
            (lambda value: value["operations"][2]["reply"]["fields"][0].__setitem__(
                "maximum", 255), "pool-status-reply"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_embedding_refusal_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][3].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][3].__setitem__("results", ["ok"]),
             "operation-results"),
            (lambda value: value["operations"][3]["reply"].__setitem__("encoded_size_ok", 35),
             "embedding-refusals-reply"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_postgres_status_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][4].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][4].__setitem__("results", ["ok"]),
             "operation-results"),
            (lambda value: value["operations"][4]["reply"].__setitem__("encoded_size_ok", 47),
             "postgres-status-reply"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_reembed_status_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][5].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][5].__setitem__("results", ["ok"]),
             "operation-results"),
            (lambda value: value["operations"][5]["reply"].__setitem__("encoded_size_ok", 35),
             "reembed-status-reply"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_reembed_clear_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][6].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][6].__setitem__("results", ["ok"]),
             "operation-results"),
            (lambda value: value["operations"][6]["reply"].__setitem__("encoded_size_ok", 25),
             "reembed-clear-reply"),
            (lambda value: value["operations"][6].__setitem__("transaction", "none"),
             "operation-semantics"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_reembed_clear_maintenance_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][7].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][7].__setitem__("results", ["ok"]),
             "operation-results"),
            (lambda value: value["operations"][7]["request"].__setitem__("encoded_size", 24),
             "reembed-clear-maintenance-request"),
            (lambda value: value["operations"][7]["reply"].__setitem__(
                "encoded_size_payload", 35), "reembed-clear-maintenance-reply"),
            (lambda value: value["operations"][7]["reply"].__setitem__(
                "payload_results", ["ok"]), "reembed-clear-maintenance-reply"),
            (lambda value: value["operations"][7].__setitem__("transaction", "none"),
             "operation-semantics"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_embedder_serving_id_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][8]["request"].__setitem__("encoded_size", 25),
             "embedder-serving-id-request"),
            (lambda value: value["operations"][8]["reply"].__setitem__(
                "encoded_size_max_ok", 188), "embedder-serving-id-reply"),
            (lambda value: value["operations"][8]["reply"]["field"].__setitem__(
                "maximum_bytes", 160), "embedder-serving-id-reply"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_dimension_reset_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][9].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][9].__setitem__("results", ["ok"]),
             "operation-results"),
            (lambda value: value["operations"][9]["request"].__setitem__("encoded_size", 35),
             "dimension-reset-request"),
            (lambda value: value["operations"][9]["request"]["fields"][0].__setitem__(
                "maximum", 4001), "dimension-reset-request"),
            (lambda value: value["operations"][9]["reply"].__setitem__(
                "encoded_size_payload", 55), "dimension-reset-reply"),
            (lambda value: value["operations"][9]["reply"].__setitem__(
                "payload_results", ["ok"]), "dimension-reset-reply"),
            (lambda value: value["operations"][9].__setitem__("transaction", "none"),
             "operation-semantics"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_descriptor_and_process_bindings_fail_closed(self) -> None:
        temporary = self.fixture()
        try:
            root = Path(temporary.name)
            descriptor_path = root / generator.DESCRIPTOR
            descriptor = json.loads(descriptor_path.read_text(encoding="utf-8"))
            descriptor["contracts"] = []
            descriptor_path.write_text(json.dumps(descriptor), encoding="utf-8")
            with self.assertRaisesRegex(generator.ContractError, "rule=descriptor-ownership"):
                generator.generated(root)

            shutil.copy2(REPO_ROOT / generator.DESCRIPTOR, descriptor_path)
            process_path = root / generator.PROCESS_CONTRACTS
            process = json.loads(process_path.read_text(encoding="utf-8"))
            db2 = next(row for row in process["components"] if row["id"] == "db2")
            db2["stages"][0]["event_kind"] = 11522
            process_path.write_text(json.dumps(process), encoding="utf-8")
            with self.assertRaisesRegex(generator.ContractError, "rule=process-activation"):
                generator.generated(root)
        finally:
            temporary.cleanup()

    def test_catalog_c_symbols_require_matching_signature_reviews(self) -> None:
        temporary = self.fixture()
        try:
            root = Path(temporary.name)
            review_path = root / generator.DECLARATION_REVIEW
            review = json.loads(review_path.read_text(encoding="utf-8"))
            row = next(item for item in review["reviews"]
                       if item["symbol"] == "db2_pool_stats")
            row["disposition"] = "private-db2"
            review_path.write_text(json.dumps(review), encoding="utf-8")
            with self.assertRaisesRegex(generator.ContractError,
                                        "rule=declaration-operation-binding"):
                generator.generated(root)
        finally:
            temporary.cleanup()

    def test_reserved_event_kind_collision_is_rejected(self) -> None:
        temporary = self.fixture()
        try:
            root = Path(temporary.name)
            path = root / generator.PROCESS_CONTRACTS
            process = json.loads(path.read_text(encoding="utf-8"))
            component = next(row for row in process["components"] if row.get("stages"))
            component["stages"][0]["event_kind"] = 11528
            path.write_text(json.dumps(process), encoding="utf-8")
            with self.assertRaisesRegex(generator.ContractError, "rule=event-kind-collision"):
                generator.generated(root)
        finally:
            temporary.cleanup()

    def test_declaration_completeness_gate_fails_closed(self) -> None:
        temporary = self.fixture()
        try:
            root = Path(temporary.name)
            review_path = root / generator.DECLARATION_REVIEW
            review = json.loads(review_path.read_text(encoding="utf-8"))
            review["declarations_complete"] = True
            review_path.write_text(json.dumps(review), encoding="utf-8")
            with self.assertRaisesRegex(generator.ContractError,
                                        "rule=declaration-completeness-drift"):
                generator.generated(root)

            shutil.copy2(REPO_ROOT / generator.DECLARATION_REVIEW, review_path)
            catalog = generator.validate_catalog(
                generator.load_json(root / generator.CATALOG))
            catalog["catalog_complete"] = True
            with self.assertRaisesRegex(generator.ContractError,
                                        "rule=catalog-declaration-gate"):
                generator._validate_declaration_gate(root, catalog)
        finally:
            temporary.cleanup()

    def test_write_then_check_and_drift_detection(self) -> None:
        temporary = self.fixture()
        try:
            root = Path(temporary.name)
            (root / generator.HEADER).unlink()
            (root / generator.CLIENT_HEADER).unlink()
            (root / generator.CLIENT_SOURCE).unlink()
            (root / generator.GO_CONTRACT).unlink()
            (root / generator.BASELINE).unlink()
            generator.run(root, True)
            generator.run(root, False)
            (root / generator.HEADER).write_text("drift\n", encoding="utf-8")
            with self.assertRaisesRegex(generator.ContractError, "rule=generated-drift"):
                generator.run(root, False)
            generator.run(root, True)
            (root / generator.GO_CONTRACT).write_text("package drift\n", encoding="utf-8")
            with self.assertRaisesRegex(generator.ContractError, "rule=generated-drift"):
                generator.run(root, False)
        finally:
            temporary.cleanup()

    def test_output_symlink_is_rejected(self) -> None:
        temporary = self.fixture()
        try:
            root = Path(temporary.name)
            header = root / generator.HEADER
            header.unlink()
            header.symlink_to(root / generator.BASELINE)
            with self.assertRaisesRegex(generator.ContractError, "rule=output-symlink"):
                generator.run(root, True)
        finally:
            temporary.cleanup()

    def test_json_input_failures_are_typed(self) -> None:
        cases = (
            (b'{"x":1,"x":2}', "json-duplicate-key"),
            (b"\xef\xbb\xbf{}", "json-bom"),
            (b'{"x":1.5}', "json-number-domain"),
            (b'{"x":NaN}', "json-number-domain"),
            (b"\xff", "json-encoding"),
            (b"{", "json-parse"),
        )
        for raw, rule in cases:
            with tempfile.TemporaryDirectory() as tmp, self.subTest(rule=rule):
                path = Path(tmp) / "input.json"
                path.write_bytes(raw)
                with self.assertRaisesRegex(generator.ContractError, rf"rule={rule}"):
                    generator.load_json(path)

    def test_json_resource_limits(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "input.json"
            path.write_bytes(b" " * (generator.MAX_BYTES + 1))
            with self.assertRaisesRegex(generator.ContractError, "rule=input-size"):
                generator.load_json(path)
            path.write_text("[" * (generator.MAX_DEPTH + 2) + "0" +
                            "]" * (generator.MAX_DEPTH + 2), encoding="utf-8")
            with self.assertRaisesRegex(generator.ContractError, "rule=json-depth"):
                generator.load_json(path)
            path.write_text(json.dumps([0] * (generator.MAX_ARRAY + 1)), encoding="utf-8")
            with self.assertRaisesRegex(generator.ContractError, "rule=json-array-size"):
                generator.load_json(path)

    def test_cli_is_cwd_independent(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run(
                [sys.executable, "-I", "-S", str(GENERATOR)],
                cwd=tmp,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("gen_db2_contract: ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
