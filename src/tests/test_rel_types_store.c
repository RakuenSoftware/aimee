/* test_rel_types_store.c: DB2-backed typed-fact store + commit path (typed-fact
 * §1 / P1b), exercised against the in-memory sqlite shim. */
#include "../headers/aimee.h"
#include "../headers/memory.h" /* edge_t (needs aimee.h first) */
#include "../db2/rel_types_store.h"
#include "../db2/entity_edges.h"
#include "../db2/entity_registry.h"
#include "../db2/db2_test_shim.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int semantic_count(const char *entity)
{
   edge_t e[64];
   return db2_entity_edges_semantic_by_entity(entity, e, 64);
}

int main(void)
{
   db2_test_shim_open();
   assert(db2_rel_types_ensure_seed() == 0);
   assert(db2_rel_types_ensure_seed() == 0); /* idempotent */

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

   /* R1-A1 read-side boundary: legacy co-occurrence reads exclude 'semantic'
    * edges. Give "zoe" one co-occurrence edge and one semantic edge, then verify
    * each recall path returns only the co-occurrence side. */
   int added = 0;
   assert(db2_entity_edge_upsert("zoe", "co_seen_with", "quux", 0, 0, 0, 0, &added) == 0);
   assert(db2_fact_commit("zoe", NODE_PERSON, "works_for", "initech", NODE_ORG,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_ACCEPT);
   edge_t ze[16];
   int m = db2_entity_edge_list_by_entity("zoe", ze, 16);
   assert(m == 1 && strcmp(ze[0].relation, "co_seen_with") == 0);
   assert(semantic_count("zoe") == 1); /* semantic recall sees only the typed edge */
   int w = db2_entity_edge_walk_step("zoe", ze, 16);
   assert(w == 1 && strcmp(ze[0].relation, "co_seen_with") == 0);
   int s = db2_entity_edge_search_by_token("zoe", ze, 16, 16);
   assert(s == 1 && strcmp(ze[0].relation, "co_seen_with") == 0);
   db2_entity_neighbor_t nb[16];
   int nn = db2_entity_edge_neighbors("zoe", nb, 16, 50);
   assert(nn == 1 && strcmp(nb[0].node, "quux") == 0);

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
