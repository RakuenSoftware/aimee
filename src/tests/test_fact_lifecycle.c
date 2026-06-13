/* test_fact_lifecycle.c: typed-fact §5 confidence classes + §4 correction/
 * retraction, against the sqlite shim. P3. */
#include "../headers/aimee.h"
#include "../db2/fact_lifecycle.h"
#include "../db2/rel_types_store.h"
#include "../db2/entity_edges.h"
#include "../db2/db2_test_shim.h"
#include "../db2/db2_internal.h"
#include "../db2/db_postgres.h"
#include "../headers/memory_ontology.h"
#include "../headers/memory_fact_gate.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
   int found;
   char cls[8];
   double conf;
   char superseded[40];
   int suppressed;
   char asserted[40];
} est_t;

/* Read the stored §4/§5 fields of a semantic edge (first match). */
static est_t edge_state(const char *src, const char *rel)
{
   est_t s;
   memset(&s, 0, sizeof(s));
   void *conn = db2_conn();
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT confidence_class, confidence, superseded_at, suppressed, asserted_at"
       " FROM entity_edges WHERE source = ?1 AND relation = ?2 AND edge_class = 'semantic' LIMIT 1",
       err, sizeof(err));
   assert(st);
   aimee_pg_bind_text(st, "?1", src);
   aimee_pg_bind_text(st, "?2", rel);
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      s.found = 1;
      snprintf(s.cls, sizeof(s.cls), "%s", aimee_pg_column_text(st, 0));
      s.conf = aimee_pg_column_double(st, 1);
      snprintf(s.superseded, sizeof(s.superseded), "%s", aimee_pg_column_text(st, 2));
      s.suppressed = aimee_pg_column_int(st, 3);
      snprintf(s.asserted, sizeof(s.asserted), "%s", aimee_pg_column_text(st, 4));
   }
   aimee_pg_finalize(st);
   return s;
}

/* Force a semantic edge's asserted_at into the past so expiry can act on it. */
static void backdate(const char *src, const char *ts)
{
   void *conn = db2_conn();
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "UPDATE entity_edges SET asserted_at = ?2 WHERE source = ?1 AND edge_class = 'semantic'",
       err, sizeof(err));
   assert(st);
   aimee_pg_bind_text(st, "?1", src);
   aimee_pg_bind_text(st, "?2", ts);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

static void test_pure(void)
{
   assert(fact_class_confidence("A") == 1.0);
   assert(fact_class_confidence("B") == 0.6);
   assert(fact_class_confidence("C") == 0.4);
   assert(fact_class_confidence("Z") == 0.4);
   assert(fact_class_confidence(NULL) == 0.4);
   assert(fact_class_rank("A") == 3 && fact_class_rank("B") == 2 && fact_class_rank("C") == 1);
   assert(fact_class_rank("x") == 0 && fact_class_rank(NULL) == 0);
   assert(strcmp(fact_class_for(FACT_AUTHORITY_USER, FACT_GATE_ACCEPT), "A") == 0);
   assert(strcmp(fact_class_for(FACT_AUTHORITY_MODEL, FACT_GATE_ACCEPT), "B") == 0);
   assert(strcmp(fact_class_for(FACT_AUTHORITY_MODEL, FACT_GATE_NOVEL), "C") == 0);
   /* a novel rel_type is speculation — Class C even from a user authority. */
   assert(strcmp(fact_class_for(FACT_AUTHORITY_USER, FACT_GATE_NOVEL), "C") == 0);
   printf("  PASS: test_pure\n");
}

int main(void)
{
   db2_test_shim_open();
   assert(db2_rel_types_ensure_seed() == 0);
   test_pure();

   /* §5 class assignment via the commit point. Model ACCEPT -> Class B. */
   assert(db2_fact_commit("alice", NODE_PERSON, "works_for", "acme", NODE_ORG, FACT_AUTHORITY_MODEL,
                          1) == FACT_GATE_ACCEPT);
   est_t s = edge_state("alice", "works_for");
   assert(s.found && strcmp(s.cls, "B") == 0 && s.conf == 0.6);
   assert(s.superseded[0] == '\0' && s.suppressed == 0 && s.asserted[0] != '\0');

   /* A user assertion on the same triple upgrades B -> A (and confidence 1.0). */
   assert(db2_fact_commit("alice", NODE_PERSON, "works_for", "acme", NODE_ORG, FACT_AUTHORITY_USER,
                          1) == FACT_GATE_ACCEPT);
   s = edge_state("alice", "works_for");
   assert(strcmp(s.cls, "A") == 0 && s.conf == 1.0);

   /* A lower-authority re-commit never downgrades A. */
   assert(db2_fact_commit("alice", NODE_PERSON, "works_for", "acme", NODE_ORG, FACT_AUTHORITY_MODEL,
                          1) == FACT_GATE_ACCEPT);
   s = edge_state("alice", "works_for");
   assert(strcmp(s.cls, "A") == 0 && s.conf == 1.0);

   /* Model NOVEL -> Class C; a user NOVEL is still C (novel = speculation). */
   assert(db2_fact_commit("bob", NODE_PERSON, "frobnicates", "thing", NODE_OTHER,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_NOVEL);
   assert(strcmp(edge_state("bob", "frobnicates").cls, "C") == 0);
   assert(db2_fact_commit("carol", NODE_PERSON, "wibbles", "y", NODE_OTHER, FACT_AUTHORITY_USER,
                          1) == FACT_GATE_NOVEL);
   assert(strcmp(edge_state("carol", "wibbles").cls, "C") == 0);

   /* §5 promotion: a Class B confirmed >= threshold times becomes durable (0.8),
    * still Class B (never A). */
   for (int i = 0; i < 3; i++)
      assert(db2_fact_commit("dave", NODE_PERSON, "works_for", "ecorp", NODE_ORG,
                             FACT_AUTHORITY_MODEL, 1) == FACT_GATE_ACCEPT);
   assert(strcmp(edge_state("dave", "works_for").cls, "B") == 0);
   assert(edge_state("dave", "works_for").conf == 0.6);
   assert(db2_fact_promote_durable(3) >= 1);
   s = edge_state("dave", "works_for");
   assert(strcmp(s.cls, "B") == 0 && s.conf == 0.8); /* durable, not promoted to A */
   assert(db2_fact_promote_durable(0) == -1);        /* bad threshold */

   /* §5 expiry: an unconfirmed (weight 1) Class C aged past the TTL is superseded;
    * a confirmed (weight>=2) Class C survives. */
   backdate("bob", "2000-01-01 00:00:00");
   assert(db2_fact_current_count("bob") == 1);
   assert(db2_fact_expire_speculative(30) >= 1);
   assert(db2_fact_current_count("bob") == 0); /* superseded, no longer current */
   assert(db2_fact_expire_speculative(0) == -1);
   /* confirm carol/wibbles (weight 2) then backdate: it must NOT expire. */
   assert(db2_fact_commit("carol", NODE_PERSON, "wibbles", "y", NODE_OTHER, FACT_AUTHORITY_MODEL,
                          1) == FACT_GATE_NOVEL);
   backdate("carol", "2000-01-01 00:00:00");
   (void)db2_fact_expire_speculative(30);
   assert(db2_fact_current_count("carol") == 1); /* confirmed C survives */

   /* §4 retract — supersede (works_for default): active edges get superseded_at,
    * row retained, not suppressed. */
   assert(db2_fact_current_count("alice") == 1);
   assert(db2_fact_retract("alice", "works_for", FACT_AUTHORITY_MODEL) >= 1);
   assert(db2_fact_current_count("alice") == 0);
   s = edge_state("alice", "works_for");
   assert(s.superseded[0] != '\0' && s.suppressed == 0);

   /* §4 retract — immutable (parent_of): model refused, user wins. */
   assert(db2_fact_commit("ann", NODE_PERSON, "parent_of", "ben", NODE_PERSON, FACT_AUTHORITY_USER,
                          1) == FACT_GATE_ACCEPT);
   assert(db2_fact_retract("ann", "parent_of", FACT_AUTHORITY_MODEL) == FACT_RETRACT_IMMUTABLE);
   assert(db2_fact_current_count("ann") == 1); /* unchanged */
   assert(db2_fact_retract("ann", "parent_of", FACT_AUTHORITY_USER) >= 1);
   assert(db2_fact_current_count("ann") == 0);

   /* §4 retract — hard_delete (also_known_as): tombstone (suppressed + superseded);
    * not immutable, so a model authority may apply it. */
   assert(db2_fact_commit("eve", NODE_PERSON, "also_known_as", "evie", NODE_OTHER,
                          FACT_AUTHORITY_USER, 1) == FACT_GATE_ACCEPT);
   assert(db2_fact_current_count("eve") == 1);
   assert(db2_fact_retract("eve", "also_known_as", FACT_AUTHORITY_MODEL) >= 1);
   s = edge_state("eve", "also_known_as");
   assert(s.suppressed == 1 && s.superseded[0] != '\0');
   assert(db2_fact_current_count("eve") == 0);

   /* bad args / no-op. */
   assert(db2_fact_retract(NULL, "works_for", FACT_AUTHORITY_MODEL) == -1);
   assert(db2_fact_retract("nobody", "works_for", FACT_AUTHORITY_MODEL) == 0);
   assert(db2_fact_current_count("") == -1);

   db2_test_shim_close();
   printf("fact_lifecycle: all tests passed\n");
   return 0;
}
