/* test_rel_types_store.c: DB2-backed typed-fact store + commit path (typed-fact
 * §1 / P1b), exercised against the in-memory sqlite shim. */
#include "../headers/aimee.h"
#include "../headers/memory.h" /* edge_t (needs aimee.h first) */
#include "../modules/db2/c/rel_types_store.h"
#include "../modules/db2/c/fact_lifecycle.h" /* db2_fact_retract */
#include "../modules/db2/c/entity_edges.h"
#include "../modules/db2/c/entity_registry.h"
#include "../modules/db2/c/db2_test_shim.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int semantic_count(const char *entity)
{
   edge_t e[64];
   return db2_entity_edges_semantic_by_entity(entity, e, 64);
}

/* Traversal reads mix both edge populations, so order is not guaranteed —
 * assert on membership rather than on a particular row landing first. */
static int edge_relation_present(const edge_t *e, int n, const char *relation)
{
   for (int i = 0; i < n; i++)
      if (strcmp(e[i].relation, relation) == 0)
         return 1;
   return 0;
}

static int neighbor_node_present(const db2_entity_neighbor_t *nb, int n, const char *node)
{
   for (int i = 0; i < n; i++)
      if (strcmp(nb[i].node, node) == 0)
         return 1;
   return 0;
}

static int check_fact_gate(int head_kind, const char *rel_type, int tail_kind, int *verdict)
{
   if (!verdict)
      return -1;
   *verdict = (int)memory_fact_gate_check((memory_node_kind_t)head_kind, rel_type,
                                          (memory_node_kind_t)tail_kind, NULL);
   return 0;
}

static int invalid_fact_gate(int head_kind, const char *rel_type, int tail_kind, int *verdict)
{
   (void)head_kind;
   (void)rel_type;
   (void)tail_kind;
   *verdict = FACT_GATE_DEFER;
   return 0;
}

static int failing_fact_gate(int head_kind, const char *rel_type, int tail_kind, int *verdict)
{
   (void)head_kind;
   (void)rel_type;
   (void)tail_kind;
   (void)verdict;
   return -1;
}

int main(void)
{
   db2_test_shim_open();
   assert(db2_rel_types_ensure_seed() == 0);
   assert(db2_rel_types_ensure_seed() == 0); /* idempotent */

   /* No provider, provider failure, and out-of-range verdicts all defer without
    * writing. The host-installed memory gate is authoritative. */
   assert(db2_fact_commit("unconfigured", NODE_PERSON, "works_for", "acme", NODE_ORG,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_DEFER);
   assert(semantic_count("unconfigured") == 0);
   aimee_db2_register_fact_gate_provider(failing_fact_gate);
   assert(db2_fact_commit("failed", NODE_PERSON, "works_for", "acme", NODE_ORG,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_DEFER);
   assert(semantic_count("failed") == 0);
   aimee_db2_register_fact_gate_provider(invalid_fact_gate);
   assert(db2_fact_commit("invalid", NODE_PERSON, "works_for", "acme", NODE_ORG,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_DEFER);
   assert(semantic_count("invalid") == 0);
   aimee_db2_register_fact_gate_provider(check_fact_gate);

   /* Resolve: seeded names (normalized), absent names. */
   long id = 0;
   assert(db2_rel_types_resolve("works_for", &id) == 1 && id > 0);
   long id2 = 0;
   assert(db2_rel_types_resolve("Works For", &id2) == 1 && id2 == id); /* normalization */
   assert(db2_rel_types_resolve("definitely_not_a_relation", NULL) == 0);

   /* ACCEPT -> a semantic edge is written. */
   assert(db2_fact_commit("alice", NODE_PERSON, "works_for", "acme", NODE_ORG, FACT_AUTHORITY_MODEL,
                          1) == FACT_GATE_ACCEPT);
   edge_t e[8];
   int n = db2_entity_edges_semantic_by_entity("alice", e, 8);
   assert(n == 1);
   assert(strcmp(e[0].relation, "works_for") == 0);
   assert(strcmp(e[0].source, "alice") == 0 && strcmp(e[0].target, "acme") == 0);

   /* REJECT_KIND -> no write. */
   assert(db2_fact_commit("printer", NODE_DEVICE, "works_for", "acme", NODE_ORG,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_REJECT_KIND);
   assert(semantic_count("printer") == 0);

   /* NOVEL -> staged provisional + a Class-C semantic edge. */
   assert(db2_fact_commit("bob", NODE_PERSON, "frobnicates", "thing", NODE_OTHER,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_NOVEL);
   assert(semantic_count("bob") == 1);
   long pid = 0;
   assert(db2_rel_types_resolve("frobnicates", &pid) == 1 && pid > 0); /* now in table */

   /* Flag off -> verdict still computed, but nothing is written. */
   assert(db2_fact_commit("carol", NODE_PERSON, "works_for", "globex", NODE_ORG,
                          FACT_AUTHORITY_MODEL, 0) == FACT_GATE_ACCEPT);
   assert(semantic_count("carol") == 0);

   /* Re-commit the same triple bumps weight, not a second row. */
   assert(db2_fact_commit("alice", NODE_PERSON, "works_for", "acme", NODE_ORG, FACT_AUTHORITY_MODEL,
                          1) == FACT_GATE_ACCEPT);
   assert(semantic_count("alice") == 1);

   /* R1-A1 read-side boundary, as split between listing and traversal.
    *
    * The boundary is about what a recall path RETURNS, not about what may serve
    * as traversal evidence: a listing/search surface still yields only the
    * co-occurrence population, while a graph walk admits both — it cares that
    * two nodes are connected, not which layer connected them.
    *
    * Give "zoe" one co-occurrence edge and one semantic edge and check each
    * path against the population it is supposed to see. */
   int added = 0;
   assert(db2_entity_edge_upsert("zoe", "co_seen_with", "quux", 0, 0, 0, 0, &added) == 0);
   assert(db2_fact_commit("zoe", NODE_PERSON, "works_for", "initech", NODE_ORG,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_ACCEPT);
   edge_t ze[16];

   /* Listing / search: co-occurrence only. */
   int m = db2_entity_edge_list_by_entity("zoe", ze, 16);
   assert(m == 1 && strcmp(ze[0].relation, "co_seen_with") == 0);
   assert(semantic_count("zoe") == 1); /* semantic recall sees only the typed edge */
   int s = db2_entity_edge_search_by_token("zoe", ze, 16, 16);
   assert(s == 1 && strcmp(ze[0].relation, "co_seen_with") == 0);

   /* Traversal: both populations. */
   int w = db2_entity_edge_walk_step("zoe", ze, 16);
   assert(w == 2);
   assert(edge_relation_present(ze, w, "co_seen_with"));
   assert(edge_relation_present(ze, w, "works_for"));
   db2_entity_neighbor_t nb[16];
   int nn = db2_entity_edge_neighbors("zoe", nb, 16, 50);
   assert(nn == 2);
   assert(neighbor_node_present(nb, nn, "quux"));
   assert(neighbor_node_present(nb, nn, "initech"));

   /* A retracted fact leaves the walk. Semantic edges carry transaction-time
    * state that co-occurrence edges do not, so admitting them to the walk is
    * only correct while the walk also constrains them to current rows —
    * otherwise the traversal starts routing through withdrawn facts. */
   assert(db2_fact_retract("zoe", "works_for", "initech", FACT_AUTHORITY_USER) == 1);
   w = db2_entity_edge_walk_step("zoe", ze, 16);
   assert(w == 1 && strcmp(ze[0].relation, "co_seen_with") == 0);
   nn = db2_entity_edge_neighbors("zoe", nb, 16, 50);
   assert(nn == 1 && strcmp(nb[0].node, "quux") == 0);
   /* The co-occurrence side is untouched by any of this. */
   assert(db2_entity_edge_list_by_entity("zoe", ze, 16) == 1);

   /* §3 endpoint resolution: an entity-kind source given via an alias is stored
    * under its canonical (preferred) name, so aliased facts share one node.
    * (Names chosen to not collide with entities registered by earlier commits.) */
   int64_t rid = db2_entity_register_named("Wilhelmina", NODE_PERSON);
   assert(rid > 0);
   assert(db2_entity_alias_bind("Billie", rid, 0) == 0);
   assert(db2_fact_commit("Billie", NODE_PERSON, "works_for", "acme", NODE_ORG,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_ACCEPT);
   assert(semantic_count("Wilhelmina") >= 1); /* stored under the canonical name */
   assert(semantic_count("Billie") == 0);     /* not under the alias */

   /* a SECOND alias collapses to the same node; a SCALAR object is stored verbatim
    * (only entity-kind endpoints are canonicalized). */
   assert(db2_entity_alias_bind("Will", rid, 0) == 0);
   assert(db2_fact_commit("Will", NODE_PERSON, "has_role", "engineer", NODE_SCALAR,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_ACCEPT);
   edge_t we[8];
   int wn = db2_entity_edges_semantic_by_entity("Wilhelmina", we, 8);
   assert(wn == 2); /* works_for + has_role, both under the canonical node */
   int found_role = 0;
   for (int i = 0; i < wn; i++)
      if (strcmp(we[i].relation, "has_role") == 0)
      {
         assert(strcmp(we[i].target, "engineer") == 0); /* scalar stored verbatim */
         found_role = 1;
      }
   assert(found_role);
   /* re-commit via an alias bumps weight on the canonical edge, no new row. */
   assert(db2_fact_commit("Billie", NODE_PERSON, "works_for", "acme", NODE_ORG,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_ACCEPT);
   assert(db2_entity_edges_semantic_by_entity("Wilhelmina", we, 8) == 2);

   db2_test_shim_close();
   printf("rel_types_store: all tests passed\n");
   return 0;
}
