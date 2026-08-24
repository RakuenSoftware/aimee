/* test_graph_fusion.c: unit tests for Phase 6 graph-vector fusion rerank. */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "db1_client/db1.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_test_shim.h"
#include "platform_test_util.h"
#include "memory.h"
#include "modules/memory/memory_graph_fusion.h"
#include "modules/db2/c/entity_edges.h"
#include "modules/db2/c/memory_query.h"

static char g_db_path[512];

static void setup(void)
{
   snprintf(g_db_path, sizeof(g_db_path), "%s/aimee-test-gf-XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(g_db_path, sizeof(g_db_path), "aim");
   assert(fd >= 0);
   close(fd);
   db2_test_shim_open_path(g_db_path);
}

static void teardown(void)
{
   db2_test_shim_close();
   platform_test_remove_sqlite(g_db_path);
   g_db_path[0] = '\0';
}

/* --- relation gravity --- */

static void test_gravity_defines_highest(void)
{
   assert(fabs(memory_graph_relation_gravity("defines") - 1.00) < 1e-9);
   assert(fabs(memory_graph_relation_gravity("contains") - 0.85) < 1e-9);
   assert(fabs(memory_graph_relation_gravity("imports") - 0.30) < 1e-9);
}

static void test_gravity_ordering(void)
{
   /* defines > contains > depends_on > routes > exports >= co_edited > calls
    * > co_discussed > imports */
   assert(memory_graph_relation_gravity("defines") > memory_graph_relation_gravity("contains"));
   assert(memory_graph_relation_gravity("contains") > memory_graph_relation_gravity("depends_on"));
   assert(memory_graph_relation_gravity("depends_on") > memory_graph_relation_gravity("routes"));
   assert(memory_graph_relation_gravity("calls") > memory_graph_relation_gravity("co_discussed"));
   assert(memory_graph_relation_gravity("co_discussed") > memory_graph_relation_gravity("imports"));
}

static void test_gravity_unknown_default(void)
{
   assert(fabs(memory_graph_relation_gravity("zzz_unknown") - 0.45) < 1e-9);
   assert(fabs(memory_graph_relation_gravity(NULL) - 0.45) < 1e-9);
}

/* --- edge score --- */

static void test_edge_score_hop_decay(void)
{
   /* 1-hop factor 1.0, 2-hop factor 0.5 — same edge, different hop. */
   double s1 = memory_graph_edge_score("defines", 1, 3, 0, 0.0, 1, NULL);
   double s2 = memory_graph_edge_score("defines", 1, 3, 0, 0.0, 2, NULL);
   assert(s1 > 0.0);
   assert(fabs(s2 - s1 * 0.5) < 1e-9);
}

static void test_edge_score_structural_factor(void)
{
   /* Code edge with structural_weight=3 gets a 2x structural factor vs
    * a non-code edge of the same relation. */
   double code_edge = memory_graph_edge_score("defines", 1, 3, 0, 0.0, 1, NULL);
   double plain_edge = memory_graph_edge_score("defines", 0, 0, 0, 0.0, 1, NULL);
   assert(code_edge > plain_edge);
   /* structural factor for sw=3 is 1 + 3/3 = 2.0 */
   assert(fabs(code_edge - plain_edge * 2.0) < 1e-9);
}

static void test_edge_score_observed_factor(void)
{
   /* Higher observed weight increases the score. */
   double w0 = memory_graph_edge_score("calls", 0, 0, 0, 0.0, 1, NULL);
   double w5 = memory_graph_edge_score("calls", 0, 0, 5, 0.0, 1, NULL);
   assert(w5 > w0);
}

static void test_edge_score_utility_dominates(void)
{
   /* Positive utility raises score; negative lowers it.
    * utility factor ranges 0.5 (at -0.5) to 3.0 (at 2.0). */
   double neg = memory_graph_edge_score("calls", 0, 0, 0, -0.5, 1, NULL);
   double zero = memory_graph_edge_score("calls", 0, 0, 0, 0.0, 1, NULL);
   double pos = memory_graph_edge_score("calls", 0, 0, 0, 2.0, 1, NULL);
   assert(neg < zero);
   assert(pos > zero);
   /* utility factor clamps: at -0.5 → 0.5, at 2.0 → 3.0 */
   assert(fabs(neg - zero * 0.5) < 1e-9);
   assert(fabs(pos - zero * 3.0) < 1e-9);
}

static void test_edge_score_utility_clamped(void)
{
   /* Beyond clamp bounds the factor saturates. */
   double huge = memory_graph_edge_score("calls", 0, 0, 0, 100.0, 1, NULL);
   double cap = memory_graph_edge_score("calls", 0, 0, 0, 2.0, 1, NULL);
   assert(fabs(huge - cap) < 1e-9);
}

/* --- confidence-class weighting (typed-fact edges) --- */

static void test_confidence_factor_ladder(void)
{
   /* A > B > C, and a co-occurrence edge (no class) is not penalised. */
   assert(fabs(memory_graph_confidence_factor("A") - 1.00) < 1e-9);
   assert(fabs(memory_graph_confidence_factor("B") - 0.75) < 1e-9);
   assert(fabs(memory_graph_confidence_factor("C") - 0.50) < 1e-9);
   assert(fabs(memory_graph_confidence_factor(NULL) - 1.00) < 1e-9);
   assert(fabs(memory_graph_confidence_factor("") - 1.00) < 1e-9);
   /* Unknown class fails conservative to the class-C weight. */
   assert(fabs(memory_graph_confidence_factor("Z") - 0.50) < 1e-9);
}

static void test_semantic_gravity_baseline(void)
{
   /* A typed fact on a relation with no table entry must outrank a
    * co-occurrence edge, not tie with it. Same weight/utility/hop throughout so
    * only the gravity and confidence terms differ. */
   double co_occurrence = memory_graph_edge_score("co_discussed", 0, 0, 1, 0.0, 1, NULL);
   double class_a = memory_graph_edge_score("works_for", 0, 0, 1, 0.0, 1, "A");
   double class_b = memory_graph_edge_score("works_for", 0, 0, 1, 0.0, 1, "B");
   double class_c = memory_graph_edge_score("works_for", 0, 0, 1, 0.0, 1, "C");

   assert(class_a > class_b);
   assert(class_b > class_c);
   assert(class_a > co_occurrence);
   /* Class A on the semantic baseline is 0.80 vs co_discussed's 0.45. */
   assert(fabs(class_a / co_occurrence - (0.80 / 0.45)) < 1e-9);
   /* Class C (0.80 * 0.5 = 0.40) lands just below a co-occurrence edge — a bare
    * speculation should not outweigh a repeatedly observed pairing. */
   assert(class_c < co_occurrence);
}

static void test_semantic_relation_keeps_table_prior(void)
{
   /* A relation that IS in the gravity table keeps its explicit prior; the
    * semantic baseline is a fallback, not an override. */
   double semantic_depends = memory_graph_edge_score("depends_on", 0, 0, 1, 0.0, 1, "A");
   double cooccur_depends = memory_graph_edge_score("depends_on", 0, 0, 1, 0.0, 1, NULL);
   assert(fabs(semantic_depends - cooccur_depends) < 1e-9);
}

/* --- code-shape detection --- */

static void test_detect_caller_flag(void)
{
   memory_query_plan_t plan;
   memset(&plan, 0, sizeof(plan));
   code_seed_reason_t r = memory_graph_detect_code_shape("anything", 1, &plan);
   assert(r == CODE_SEED_CALLER);
   assert(plan.allow_code_graph == 1);
   assert(plan.code_seed_reason == CODE_SEED_CALLER);
}

static void test_detect_path_token(void)
{
   memory_query_plan_t plan;
   memset(&plan, 0, sizeof(plan));
   code_seed_reason_t r = memory_graph_detect_code_shape("where is src/memory.c defined", 0, &plan);
   assert(r == CODE_SEED_TOKEN);
   assert(plan.allow_code_graph == 1);
}

static void test_detect_symbol_syntax(void)
{
   memory_query_plan_t plan;
   memset(&plan, 0, sizeof(plan));
   code_seed_reason_t r = memory_graph_detect_code_shape("how does Foo::bar work", 0, &plan);
   assert(r == CODE_SEED_TOKEN);
}

static void test_detect_line_ref(void)
{
   memory_query_plan_t plan;
   memset(&plan, 0, sizeof(plan));
   code_seed_reason_t r = memory_graph_detect_code_shape("bug at parser.c:42", 0, &plan);
   assert(r == CODE_SEED_TOKEN);
}

static void test_detect_non_code(void)
{
   memory_query_plan_t plan;
   memset(&plan, 0, sizeof(plan));
   code_seed_reason_t r =
       memory_graph_detect_code_shape("what did we decide about deploys", 0, &plan);
   assert(r == CODE_SEED_NONE);
   assert(plan.allow_code_graph == 0);
}

static void test_detect_extension_token(void)
{
   memory_query_plan_t plan;
   memset(&plan, 0, sizeof(plan));
   code_seed_reason_t r = memory_graph_detect_code_shape("explain handler.go", 0, &plan);
   assert(r == CODE_SEED_TOKEN);
}

/* --- expansion gating (no DB needed for guards) --- */

static void test_expand_null_seeds(void)
{
   memory_graph_expansion_t out[4];
   int n = memory_graph_expand_from_seeds(NULL, 0, 2, 16, 1, 0, out, 4);
   assert(n == 0);
}

static void test_expand_non_code_query_skips_code_seed(void)
{
   setup();
   /* allow_code_graph=0 → a code node seed must not expand. */
   const char *seeds[] = {"file:aimee:src/memory.c"};
   memory_graph_expansion_t out[4];
   int n = memory_graph_expand_from_seeds(seeds, 1, 2, 16, 0, 0, out, 4);
   assert(n == 0); /* gated out */
   teardown();
}

/* --- score parts plumbing --- */

static void test_populate_score_parts(void)
{
   memory_graph_expansion_t exp[2];
   memset(exp, 0, sizeof(exp));
   exp[0].memory_id = 42;
   exp[0].graph_score = 1.5;
   snprintf(exp[0].via, sizeof(exp[0].via), "file:aimee:src/x.c");
   exp[1].memory_id = 99;
   exp[1].graph_score = 0.7;
   snprintf(exp[1].via, sizeof(exp[1].via), "concept:deploy");

   memory_score_parts_t parts;
   memset(&parts, 0, sizeof(parts));
   memory_graph_populate_score_parts(&parts, 42, exp, 2);
   assert(fabs(parts.graph_score - 1.5) < 1e-9);
   /* file: node is code → code_proximity also set */
   assert(fabs(parts.code_proximity - 1.5) < 1e-9);

   memory_score_parts_t parts2;
   memset(&parts2, 0, sizeof(parts2));
   memory_graph_populate_score_parts(&parts2, 99, exp, 2);
   assert(fabs(parts2.graph_score - 0.7) < 1e-9);
   /* concept: node is not code → code_proximity stays 0 */
   assert(parts2.code_proximity == 0.0);
}

static void test_point_id_to_node_key_no_db(void)
{
   char nk[512];
   int rc = memory_graph_point_id_to_node_key(12345, nk, sizeof(nk));
   /* No matching row / no DB → -1, must not crash. */
   assert(rc == -1 || rc == 0);
}

/* --- Phase 6/7 acceptance: synthetic seeded end-to-end fusion fixture ---
 *
 * Proves a query whose best answer is only connected through a code symbol
 * can retrieve the relevant memory through graph expansion. Uses a seeded
 * in-process SQLite-shim DB, NOT the live DB (per the proposal). */
static void test_e2e_code_symbol_bridges_to_memory(void)
{
   setup();

   /* A memory that documents a decision, linked in the graph to a code symbol
    * rather than mentioning the symbol in its text. */
   memory_t mem;
   int rc = memory_insert(TIER_L2, KIND_DECISION, "retry-policy",
                          "We cap delegate retries at three attempts.", 0.9, "sess1", &mem);
   assert(rc == 0 && mem.id > 0);

   /* Link the memory to the canonical code symbol node via memory_entities. */
   const char *sym = "symbol:aimee:agent_resolve_max_turns";
   db2_memory_entity_insert(mem.id, sym, "mentions", 2.0);

   /* A code-projection edge from a file to that symbol (defines). */
   int added = 0;
   db2_entity_edge_upsert("file:aimee:src/agent.c", "defines", sym, 0, (int)5 /*REL_CALLS*/, 0, 1,
                          &added);

   /* Expand from the code symbol seed with code traversal allowed. */
   const char *seeds[] = {sym};
   memory_graph_expansion_t out[8];
   int n = memory_graph_expand_from_seeds(seeds, 1, 2, 16, /*allow_code_graph=*/1,
                                          /*utility_scoring=*/0, out,
                                          (int)(sizeof(out) / sizeof(out[0])));

   /* The seeded memory must be reachable through the symbol bridge, in top results. */
   int found = 0;
   for (int i = 0; i < n && i < 10; i++)
      if (out[i].memory_id == mem.id)
      {
         found = 1;
         assert(out[i].graph_score > 0.0);
      }
   assert(found && "memory reachable through code symbol in top 10");

   teardown();
}

/* Companion negative: a non-code query (allow_code_graph=0) must NOT reach the
 * memory through the code symbol — gating holds end to end. */
static void test_e2e_non_code_query_gated_out(void)
{
   setup();
   memory_t mem;
   assert(memory_insert(TIER_L2, KIND_DECISION, "retry-policy2", "Retry cap rationale.", 0.9,
                        "sess1", &mem) == 0);
   const char *sym = "symbol:aimee:agent_resolve_max_turns";
   db2_memory_entity_insert(mem.id, sym, "mentions", 2.0);
   int added = 0;
   db2_entity_edge_upsert("file:aimee:src/agent.c", "defines", sym, 0, 5, 0, 1, &added);

   const char *seeds[] = {sym};
   memory_graph_expansion_t out[8];
   int n = memory_graph_expand_from_seeds(seeds, 1, 2, 16, /*allow_code_graph=*/0, 0, out,
                                          (int)(sizeof(out) / sizeof(out[0])));
   /* Code seed gated out → no expansion results. */
   assert(n == 0);
   teardown();
}

/* --- recall-path fusion state (thread-local plumbing, no DB) --- */

static void test_fusion_state_on_off(void)
{
   memory_fusion_state_set("on");
   assert(memory_fusion_state_is_on() == 1);
   /* "shadow"/"off"/unknown/NULL are all not-on (shadow capture is a follow-up). */
   memory_fusion_state_set("shadow");
   assert(memory_fusion_state_is_on() == 0);
   memory_fusion_state_set("off");
   assert(memory_fusion_state_is_on() == 0);
   memory_fusion_state_set("bogus");
   assert(memory_fusion_state_is_on() == 0);
   memory_fusion_state_set(NULL);
   assert(memory_fusion_state_is_on() == 0);
   memory_fusion_state_set("on");
   memory_fusion_state_clear();
   assert(memory_fusion_state_is_on() == 0);
}

static void test_fusion_expansions_apply(void)
{
   memory_graph_expansion_t exp[1];
   memset(exp, 0, sizeof(exp));
   exp[0].memory_id = 7;
   exp[0].graph_score = 2.5;
   snprintf(exp[0].via, sizeof(exp[0].via), "symbol:p:foo");

   /* No expansions staged → apply is a no-op (ranking stays byte-identical). */
   memory_score_parts_t parts;
   memset(&parts, 0, sizeof(parts));
   memory_fusion_expansions_apply(&parts, 7);
   assert(parts.graph_score == 0.0);

   /* Staged → the matching memory_id gets its graph_score populated. */
   memory_fusion_expansions_set(exp, 1);
   memory_fusion_expansions_apply(&parts, 7);
   assert(fabs(parts.graph_score - 2.5) < 1e-9);

   /* A non-matching id is unaffected. */
   memory_score_parts_t parts2;
   memset(&parts2, 0, sizeof(parts2));
   memory_fusion_expansions_apply(&parts2, 999);
   assert(parts2.graph_score == 0.0);

   /* After clear, apply is a no-op again. */
   memory_fusion_expansions_clear();
   memory_score_parts_t parts3;
   memset(&parts3, 0, sizeof(parts3));
   memory_fusion_expansions_apply(&parts3, 7);
   assert(parts3.graph_score == 0.0);
}

static void test_fusion_gates(void)
{
   /* Defaults are on (so graph_code_fusion_state=on runs full fusion). */
   memory_fusion_state_clear();
   assert(memory_fusion_utility_scoring() == 1);
   assert(memory_fusion_code_projection() == 1);
   /* An arm can turn each sub-gate off independently. */
   memory_fusion_gates_set(0, 1);
   assert(memory_fusion_utility_scoring() == 0);
   assert(memory_fusion_code_projection() == 1);
   memory_fusion_gates_set(1, 0);
   assert(memory_fusion_utility_scoring() == 1);
   assert(memory_fusion_code_projection() == 0);
   /* clear resets both gates back to the default (on). */
   memory_fusion_state_clear();
   assert(memory_fusion_utility_scoring() == 1);
   assert(memory_fusion_code_projection() == 1);
}

/* db2_entity_edge_explain_by_entity returns full provenance for incident edges. */
static void test_explain_read_provenance(void)
{
   setup();
   int added = 0;
   db2_entity_edge_upsert("file:p:a.c", "defines", "symbol:p:foo", 0, 5, 0, 1, &added);
   db2_entity_edge_explain_t rows[16];
   int n = db2_entity_edge_explain_by_entity("symbol:p:foo", rows, 16);
   assert(n >= 1);
   int seen = 0;
   for (int i = 0; i < n; i++)
      if (strcmp(rows[i].relation, "defines") == 0 && strcmp(rows[i].source, "file:p:a.c") == 0)
         seen = 1;
   assert(seen);
   teardown();
}

int main(void)
{
   printf("test_gravity_defines_highest... ");
   test_gravity_defines_highest();
   printf("ok\n");
   printf("test_gravity_ordering... ");
   test_gravity_ordering();
   printf("ok\n");
   printf("test_gravity_unknown_default... ");
   test_gravity_unknown_default();
   printf("ok\n");
   printf("test_edge_score_hop_decay... ");
   test_edge_score_hop_decay();
   printf("ok\n");
   printf("test_edge_score_structural_factor... ");
   test_edge_score_structural_factor();
   printf("ok\n");
   printf("test_edge_score_observed_factor... ");
   test_edge_score_observed_factor();
   printf("ok\n");
   printf("test_edge_score_utility_dominates... ");
   test_edge_score_utility_dominates();
   printf("ok\n");
   printf("test_edge_score_utility_clamped... ");
   test_edge_score_utility_clamped();
   printf("ok\n");
   printf("test_confidence_factor_ladder... ");
   test_confidence_factor_ladder();
   printf("ok\n");
   printf("test_semantic_gravity_baseline... ");
   test_semantic_gravity_baseline();
   printf("ok\n");
   printf("test_semantic_relation_keeps_table_prior... ");
   test_semantic_relation_keeps_table_prior();
   printf("ok\n");
   printf("test_detect_caller_flag... ");
   test_detect_caller_flag();
   printf("ok\n");
   printf("test_detect_path_token... ");
   test_detect_path_token();
   printf("ok\n");
   printf("test_detect_symbol_syntax... ");
   test_detect_symbol_syntax();
   printf("ok\n");
   printf("test_detect_line_ref... ");
   test_detect_line_ref();
   printf("ok\n");
   printf("test_detect_non_code... ");
   test_detect_non_code();
   printf("ok\n");
   printf("test_detect_extension_token... ");
   test_detect_extension_token();
   printf("ok\n");
   printf("test_expand_null_seeds... ");
   test_expand_null_seeds();
   printf("ok\n");
   printf("test_expand_non_code_query_skips_code_seed... ");
   test_expand_non_code_query_skips_code_seed();
   printf("ok\n");
   printf("test_populate_score_parts... ");
   test_populate_score_parts();
   printf("ok\n");
   printf("test_point_id_to_node_key_no_db... ");
   test_point_id_to_node_key_no_db();
   printf("ok\n");
   printf("test_e2e_code_symbol_bridges_to_memory... ");
   test_e2e_code_symbol_bridges_to_memory();
   printf("ok\n");
   printf("test_e2e_non_code_query_gated_out... ");
   test_e2e_non_code_query_gated_out();
   printf("ok\n");
   printf("test_explain_read_provenance... ");
   test_explain_read_provenance();
   printf("ok\n");
   printf("test_fusion_state_on_off... ");
   test_fusion_state_on_off();
   printf("ok\n");
   printf("test_fusion_expansions_apply... ");
   test_fusion_expansions_apply();
   printf("ok\n");
   printf("test_fusion_gates... ");
   test_fusion_gates();
   printf("ok\n");
   printf("graph_fusion: all tests passed\n");
   return 0;
}
