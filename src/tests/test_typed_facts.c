/* test_typed_facts.c: typed-fact store + write gate over the sqlite shim —
 * ontology gate, kind validation, contradiction-supersede, recall. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "modules/db2/c/db2_test_shim.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db_postgres.h"
#include "modules/db2/c/memory_scope_query.h"
#include "../modules/db2/c/typed_facts.h"
#include "../modules/db2/c/fact_recall.h"     /* db2_fact_recall_block */
#include "../modules/db2/c/fact_lifecycle.h"  /* FACT_AUTHORITY_MODEL */
#include "../modules/db2/c/rel_types_store.h" /* db2_fact_commit */
#include "modules/memory/memory_fact_gate.h"  /* FACT_GATE_NOVEL */
#include "modules/memory/memory_ontology.h"   /* NODE_PERSON, NODE_OTHER */

static int check_fact_gate(int head_kind, const char *rel_type, int tail_kind, int *verdict)
{
   if (!verdict)
      return -1;
   *verdict = (int)memory_fact_gate_check((memory_node_kind_t)head_kind, rel_type,
                                          (memory_node_kind_t)tail_kind, NULL);
   return 0;
}

int main(void)
{
   db2_test_shim_open();
   aimee_db2_register_fact_gate_provider(check_fact_gate);

   const char *T = "2026-01-01T00:00:00Z";

   /* ontology membership */
   assert(typed_fact_relation_known("naming_convention"));
   assert(!typed_fact_relation_known("totally_made_up"));

   /* assert a convention fact */
   assert(db2_typed_fact_assert("fizzy", "project", "naming_convention", "BEM", "scalar", 90,
                                "exemplar-scan", T) == TYPED_FACT_OK);

   typed_fact_t f[16];
   int n = db2_typed_fact_recall("fizzy", "naming_convention", f, 16);
   assert(n == 1);
   assert(strcmp(f[0].object, "BEM") == 0 && f[0].confidence == 90);
   assert(strcmp(f[0].source, "exemplar-scan") == 0);

   /* idempotent re-assert of the identical fact */
   assert(db2_typed_fact_assert("fizzy", "project", "naming_convention", "BEM", "scalar", 90,
                                "exemplar-scan", T) == TYPED_FACT_UNCHANGED);
   assert(db2_typed_fact_recall("fizzy", "naming_convention", f, 16) == 1);

   /* contradiction supersedes the prior value (history retained, only 1 active) */
   assert(db2_typed_fact_assert("fizzy", "project", "naming_convention", "utility", "scalar", 80,
                                "human-correction", T) == TYPED_FACT_OK);
   n = db2_typed_fact_recall("fizzy", "naming_convention", f, 16);
   assert(n == 1 && strcmp(f[0].object, "utility") == 0); /* new value wins; old superseded */

   /* unknown relation is rejected by the gate */
   assert(db2_typed_fact_assert("fizzy", "project", "vibes", "good", "scalar", 50, "x", T) ==
          TYPED_FACT_REJECTED_REL);

   /* kind mismatch rejected: should_match needs subject_kind=component */
   assert(db2_typed_fact_assert("fizzy", "project", "should_match", "conv", "convention", 50, "x",
                                T) == TYPED_FACT_REJECTED_KIND);

   /* per-component should_match facts + by_relation lookup */
   assert(db2_typed_fact_assert("Button.tsx", "component", "should_match", "btn_convention",
                                "convention", 70, "driver", T) == TYPED_FACT_OK);
   assert(db2_typed_fact_assert("Card.tsx", "component", "should_match", "card_convention",
                                "convention", 70, "driver", T) == TYPED_FACT_OK);
   n = db2_typed_fact_by_relation("should_match", f, 16);
   assert(n == 2);

   /* recall all relations for a subject */
   assert(db2_typed_fact_assert("fizzy", "project", "token_strategy", "css-vars", "scalar", 85, "x",
                                T) == TYPED_FACT_OK);
   n = db2_typed_fact_recall("fizzy", NULL, f, 16);
   assert(n == 2); /* naming_convention(utility) + token_strategy */

   /* db2_fact_commit path (entity_edges) — the one the memory-fact extractor and
    * auto-inject use. Unlike the strict CSS assert above, this gate ACCEPTs a
    * free-form (NOVEL) relation as a provisional semantic edge. Model-derived
    * assertions remain candidates and therefore stay out of default recall. */
   assert(db2_fact_commit("user", NODE_PERSON, "works_as", "engineer", NODE_OTHER,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_NOVEL);
   /* Candidate quarantine: inference is reviewable, but not recallable. */
   char facts[1024] = "";
   int fn = db2_fact_recall_block("user", 0, facts, sizeof(facts));
   assert(fn == 0 && strstr(facts, "works_as") == NULL);
   /* Authenticated-user evidence promotes the exact candidate to persistent. */
   assert(db2_fact_commit("user", NODE_PERSON, "works_as", "engineer", NODE_OTHER,
                          FACT_AUTHORITY_USER, 1) == FACT_GATE_NOVEL);
   fn = db2_fact_recall_block("user", 0, facts, sizeof(facts));
   assert(fn >= 1 && strstr(facts, "works_as") != NULL && strstr(facts, "engineer") != NULL);

   /* But an unknown relation whose NAME plainly denotes PII is still gated:
    * withheld on an ordinary turn, surfaced only when the turn asks. (PII facts
    * remain in the typed-fact layer, recall-gated — only credentials are dropped;
    * see the api_key assertion below.) */
   assert(db2_fact_commit("user", NODE_PERSON, "home_address", "12 Oak St", NODE_OTHER,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_NOVEL);
   char ord[1024] = "";
   (void)db2_fact_recall_block("user", 0, ord, sizeof(ord));
   assert(strstr(ord, "home_address") == NULL); /* PII-looking: withheld by default */
   char sens[1024] = "";
   (void)db2_fact_recall_block("user", 1, sens, sizeof(sens));
   assert(strstr(sens, "home_address") == NULL); /* candidate stays quarantined */
   assert(db2_fact_commit("user", NODE_PERSON, "home_address", "12 Oak St", NODE_OTHER,
                          FACT_AUTHORITY_USER, 1) == FACT_GATE_NOVEL);
   (void)db2_fact_recall_block("user", 1, sens, sizeof(sens));
   assert(strstr(sens, "home_address") != NULL); /* persistent and explicitly requested */

   /* Personal-data boundary (Track A): a CREDENTIAL relation is withheld from the
    * shared KB entirely — never committed to DB2, so it never surfaces there. */
   assert(db2_fact_commit("user", NODE_PERSON, "api_key", "sk-123", NODE_OTHER,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_REJECT_SENSITIVE);
   char cred[1024] = "";
   (void)db2_fact_recall_block("user", 1, cred, sizeof(cred));
   assert(strstr(cred, "api_key") == NULL); /* not in the shared KB, even when asked */

   /* The semantic channel applies valid-time and transaction-time independently,
    * labels old versions, retains exact evidence locators, and rejects malformed
    * time input rather than falling back to unfiltered history. */
   char err[256] = "";
   int sql_rc =
       aimee_pg_exec(db2_conn(),
                     "INSERT INTO fact_graph_commits"
                     " (commit_id,operation,actor_principal,actor_role,authority_rank,status)"
                     " VALUES ('temporal-fixture','assert','test','system',100,'open')",
                     err, sizeof(err));
   assert(sql_rc == 0);
   const char *temporal_fixture_sql =
       "BEGIN;"
       "INSERT INTO entity_edges"
       " (id,source,relation,target,edge_class,assertion_kind,lifecycle_state,confidence_class,"
       " confidence,authority_rank,valid_from,valid_until,asserted_at,superseded_at,commit_id) "
       "VALUES"
       " (9001,'Atlas','deployment_state','old','semantic','world_fact','persistent','A',"
       "  0.9,80,'2026-01-01T00:00:00Z','2026-03-01T00:00:00Z',"
       " '2026-01-02T00:00:00Z','2026-03-02T00:00:00Z','temporal-fixture'),"
       " (9002,'Atlas','deployment_state','new','semantic','world_fact','persistent','A',"
       "  0.95,80,'2026-03-01T00:00:00Z','','2026-03-02T00:00:00Z','',"
       " 'temporal-fixture');"
       "INSERT INTO fact_graph_changes"
       " (commit_id,assertion_id,action,existed_before,existed_after,after_lifecycle,"
       " after_confidence,after_authority_rank,after_version) VALUES"
       " ('temporal-fixture',9001,'assert',0,1,'persistent',0.9,80,1),"
       " ('temporal-fixture',9002,'assert',0,1,'persistent',0.95,80,1);"
       "COMMIT";
   sql_rc = aimee_pg_exec(db2_conn(), temporal_fixture_sql, err, sizeof(err));
   if (sql_rc != 0)
      fprintf(stderr, "semantic fixture insert failed: %s\n", err);
   assert(sql_rc == 0);
   assert(aimee_pg_exec(
              db2_conn(),
              "INSERT INTO fact_evidence"
              " (assertion_id,source_kind,source_id,source_span,evidence_hash,observed_at,stance)"
              " VALUES (9001,'episode','event:41','bytes:4-19','abc',"
              " '2026-01-02T00:00:00Z','supports')",
              err, sizeof(err)) == 0);
   semantic_assertion_hit_t hits[4];
   n = db2_semantic_assertion_search("atlas", "2026-02-01T00:00:00Z", "2026-02-02T00:00:00Z", 0, 4,
                                     hits, 4);
   assert(n == 1 && hits[0].assertion_id == 9001);
   assert(hits[0].historical == 1 && hits[0].evidence_count == 1);
   assert(hits[0].retrieval_count == 1);
   assert(strcmp(hits[0].retrieval[0].channel, "lexical") == 0);
   assert(strcmp(hits[0].evidence[0].source_span, "bytes:4-19") == 0);
   n = db2_semantic_assertion_search("atlas", "2026-04-01T00:00:00Z", "2026-04-02T00:00:00Z", 0, 4,
                                     hits, 4);
   assert(n == 1 && hits[0].assertion_id == 9002 && hits[0].historical == 0);
   semantic_assertion_hit_t by_id;
   assert(db2_semantic_assertion_get_filtered(9002, "2026-04-01T00:00:00Z", "2026-04-02T00:00:00Z",
                                              0, &by_id) == 1);
   assert(strcmp(by_id.object, "new") == 0);
   assert(db2_semantic_assertion_search("atlas", "2026-04-01T00:00:00.123Z", "", 0, 4, hits, 4) ==
          SEMANTIC_ASSERTION_SEARCH_INVALID_TIME);

   /* A derived assertion is visible only if every scoped memory evidence item
    * is visible in the request-local partition. */
   assert(aimee_pg_exec(db2_conn(),
                        "INSERT INTO memories(id,key,content,scope_type,scope_value)"
                        " VALUES(9100,'scoped','evidence','project','allowed-project');"
                        "INSERT INTO memory_scopes(memory_id,scope_type,scope_value)"
                        " VALUES(9100,'project','allowed-project');"
                        "INSERT INTO fact_evidence"
                        " (assertion_id,source_kind,source_id,source_span,evidence_hash,stance)"
                        " VALUES(9002,'memory','memory:9100','bytes:0-8','scope-hash','supports')",
                        err, sizeof(err)) == 0);
   db2_memory_scope_context_set("", "different-project", 0);
   assert(db2_semantic_assertion_search("atlas", "2026-04-01T00:00:00Z", "2026-04-02T00:00:00Z", 0,
                                        4, hits, 4) == 0);
   db2_memory_scope_context_set("", "allowed-project", 0);
   assert(db2_semantic_assertion_search("atlas", "2026-04-01T00:00:00Z", "2026-04-02T00:00:00Z", 0,
                                        4, hits, 4) == 1);
   db2_memory_scope_context_clear();

   /* Canonical rendering is derived from the single assertion store; versioned
    * vector state suppresses already-current rows without duplicating truth. */
   semantic_assertion_index_row_t index_rows[4];
   int indexed = db2_semantic_assertion_index_list(0, index_rows, 4);
   assert(indexed >= 2);
   int saw_canonical_rendering = 0;
   for (int i = 0; i < indexed; i++)
      saw_canonical_rendering |= index_rows[i].canonical_rendering[0] != '\0';
   assert(saw_canonical_rendering);
   assert(aimee_pg_exec(db2_conn(),
                        "INSERT INTO memory_embeddings"
                        " (point_id,record_type,kind,payload_json)"
                        " VALUES(2000000009001,'semantic_assertion','assertion_v1','{}')",
                        err, sizeof(err)) == 0);
   indexed = db2_semantic_assertion_index_list(9000, index_rows, 4);
   for (int i = 0; i < indexed; i++)
      assert(index_rows[i].assertion_id != 9001);

   db2_test_shim_close();
   printf("typed_facts: all tests passed\n");
   return 0;
}
