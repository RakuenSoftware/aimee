#!/usr/bin/env python3
"""Structural tests for DB2's transactional DB3 projection ledger."""

from __future__ import annotations

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCHEMA = ROOT / "src/modules/db2/c/schema.sql"
TRANSPORT = ROOT / "src/modules/db2/c/pgvec_transport.c"


class DB3OutboxSchemaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.schema = SCHEMA.read_text(encoding="utf-8")
        cls.transport = TRANSPORT.read_text(encoding="utf-8")

    def test_delivery_ledger_is_separate_from_pgvector_retry_bookkeeping(self) -> None:
        for table in (
            "db3_projection", "db3_provider", "db3_outbox", "db3_delivery",
            "db3_backfill",
        ):
            self.assertEqual(
                len(re.findall(rf"CREATE TABLE IF NOT EXISTS {table}\b", self.schema)), 1,
                table,
            )
        self.assertIn("CREATE TABLE IF NOT EXISTS vector_index_ops", self.schema)
        outbox = self.schema[self.schema.index("CREATE TABLE IF NOT EXISTS db3_outbox"):]
        outbox = outbox[:outbox.index("CREATE TABLE IF NOT EXISTS db3_delivery")]
        self.assertNotIn("vector_index_ops", outbox)
        self.assertIn("PRIMARY KEY(operation_id, principal)", self.schema)
        self.assertIn("REFERENCES db3_outbox(operation_id) ON DELETE CASCADE", self.schema)

    def test_admission_live_capture_and_chunked_backfill_share_one_lock(self) -> None:
        lock = "pg_advisory_xact_lock(1144209987)"
        self.assertEqual(self.schema.count(lock), 3)
        enqueue = self.schema.index("CREATE OR REPLACE FUNCTION db3_enqueue_vector_to")
        capture = self.schema.index("CREATE OR REPLACE FUNCTION db3_capture_vector_row")
        setup = self.schema.index("DO $pgvec_setup$")
        admit = self.schema.index("CREATE OR REPLACE FUNCTION db3_admit_provider")
        backfill = self.schema.index("CREATE OR REPLACE FUNCTION db3_backfill_provider_chunk")
        self.assertLess(enqueue, capture)
        self.assertLess(capture, setup)
        self.assertLess(setup, admit)
        self.assertLess(admit, backfill)
        self.assertIn("p_limit NOT BETWEEN 1 AND 256", self.schema)
        self.assertIn("ORDER BY projection_id LIMIT 1 FOR UPDATE", self.schema)
        self.assertIn("WHERE principal=p_principal FOR UPDATE", self.schema)
        admit_body = self.schema[admit:backfill]
        self.assertNotIn("FOR r IN EXECUTE", admit_body)

    def test_no_provider_has_no_unbounded_history(self) -> None:
        enqueue = self.schema[
            self.schema.index("CREATE OR REPLACE FUNCTION db3_enqueue_vector_to"):
            self.schema.index("CREATE OR REPLACE FUNCTION db3_enqueue_vector(")
        ]
        self.assertIn("SELECT max(corpus_generation) INTO v_generation", enqueue)
        self.assertIn("IF v_generation IS NULL THEN", enqueue)
        self.assertLess(enqueue.index("IF v_generation IS NULL THEN"),
                        enqueue.index("INSERT INTO public.db3_outbox"))

    def test_every_portable_pgvector_mutation_relation_has_row_and_truncate_triggers(self) -> None:
        relations = {
            match.group(1)
            for match in re.finditer(
                r'(?:INSERT INTO|DELETE FROM) '
                r'(memory_embeddings|kb_embeddings|kb_pdf_embeddings|'
                r'curator_entity_vectors|curator_narrative_vectors|'
                r'curator_claim_vectors|curator_code_unit_vectors|code_embeddings)\b',
                self.transport,
            )
        }
        self.assertEqual(relations, {
            "memory_embeddings", "kb_embeddings", "kb_pdf_embeddings",
            "curator_entity_vectors", "curator_narrative_vectors",
            "curator_claim_vectors", "curator_code_unit_vectors", "code_embeddings",
        })
        trigger_block = self.schema[self.schema.index("INSERT INTO public.db3_projection("):
                                    self.schema.index("END $pgvec_setup$")]
        for relation in relations:
            self.assertIn(f"'{relation}'", trigger_block)
        self.assertEqual(trigger_block.count("INSERT INTO public.db3_projection("), 1)
        self.assertIn("SELECT DISTINCT relation_name FROM public.db3_projection", trigger_block)
        self.assertIn("SELECT * FROM public.db3_projection ORDER BY projection_id", trigger_block)
        self.assertIn("AFTER INSERT OR UPDATE OR DELETE", trigger_block)
        self.assertIn("BEFORE TRUNCATE", trigger_block)
        self.assertIn("DB3_TRUNCATE_REQUIRES_REBUILD", self.schema)

    def test_one_projection_catalog_drives_capture_trigger_and_backfill(self) -> None:
        catalog = self.schema[self.schema.index("INSERT INTO public.db3_projection("):
                              self.schema.index("END $pgvec_setup$")]
        relations = (
            "memory_embeddings", "kb_embeddings", "kb_pdf_embeddings",
            "curator_entity_vectors", "curator_narrative_vectors",
            "curator_claim_vectors", "curator_code_unit_vectors", "code_embeddings",
        )
        collections = (
            "memory", "kb", "kb_pdf", "curator_entity", "curator_narrative",
            "curator_claim_subject", "curator_claim_value",
            "curator_code_intent", "curator_code_signature", "curator_code_body", "code",
        )
        for relation in relations:
            self.assertIn(f"'{relation}'", catalog)
        for collection in collections:
            self.assertRegex(catalog, rf"'{re.escape(collection)}'")
        capture = self.schema[self.schema.index("CREATE OR REPLACE FUNCTION db3_capture_vector_row"):
                              self.schema.index("CREATE OR REPLACE FUNCTION db3_reject_vector_truncate")]
        backfill = self.schema[self.schema.index("CREATE OR REPLACE FUNCTION db3_backfill_provider_chunk"):
                               self.schema.index("/* ---- graph reasoning")]
        for body in (capture, backfill):
            self.assertIn("public.db3_projection", body)
            for relation in relations:
                self.assertNotIn(f"'{relation}'", body)
            for collection in collections:
                self.assertNotIn(f"'{collection}'", body)
        self.assertIn("projection.vector_column", backfill)
        self.assertIn("projection.relation_name", backfill)
        self.assertIn("projection.label_sources", backfill)

    def test_nullable_vectors_are_independent_projection_points(self) -> None:
        catalog = self.schema[self.schema.index("INSERT INTO public.db3_projection("):
                              self.schema.index("END $pgvec_setup$")]
        for column in (
            "embedding", "subj_attr_vec", "value_vec", "intent_vec", "signature_vec",
            "body_vec",
        ):
            self.assertIn(f"'{column}'", catalog)
        capture = self.schema[self.schema.index("CREATE OR REPLACE FUNCTION db3_capture_vector_row"):
                              self.schema.index("CREATE OR REPLACE FUNCTION db3_reject_vector_truncate")]
        self.assertIn("v_row->>p.vector_column", capture)
        self.assertIn("IF TG_OP<>'DELETE' AND v_vector IS NOT NULL", capture)
        self.assertIn("IF v_row->>p.vector_column IS NOT NULL", capture)

    def test_label_bytes_are_canonicalized_from_catalog_keys(self) -> None:
        labels = self.schema[self.schema.index("CREATE OR REPLACE FUNCTION db3_projection_labels"):
                             self.schema.index("CREATE OR REPLACE FUNCTION db3_enqueue_vector_to")]
        self.assertIn("jsonb_object_agg", labels)
        self.assertIn("ORDER BY source.key", labels)
        self.assertIn("public.db3_projection_labels(v_row,p.label_sources)", self.schema)

    def test_contract_bounds_and_atomic_failure_are_database_enforced(self) -> None:
        enqueue = self.schema[
            self.schema.index("CREATE OR REPLACE FUNCTION db3_enqueue_vector_to"):
            self.schema.index("CREATE OR REPLACE FUNCTION db3_enqueue_vector(")
        ]
        self.assertIn("(SELECT count(*) FROM jsonb_each(p_labels))>16", enqueue)
        self.assertIn("octet_length(value)>255", enqueue)
        self.assertIn("FROM jsonb_each_text(p_labels))>4096", enqueue)
        self.assertIn("DB3_OUTBOX_CONTRACT", enqueue)
        self.assertNotIn("EXCEPTION WHEN OTHERS", enqueue)
        self.assertIn("AFTER INSERT OR UPDATE OR DELETE", self.schema)


if __name__ == "__main__":
    unittest.main()
