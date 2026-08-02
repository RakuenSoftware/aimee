/* test_entity_registry.c: surrogate-id entity canonicalization (typed-fact §3 /
 * P2a), against the sqlite shim. */
#include "../headers/aimee.h"
#include "../db2/entity_registry.h"
#include "../db2/db2_test_shim.h"
#include "../db2/db2_internal.h" /* db2_conn */
#include "../db2/db_postgres.h" /* aimee_pg_* (migration test) */
#include "memory_ontology.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_normalize(void)
{
   char out[64];
   entity_name_normalize("  DevBox  ", out, sizeof(out));
   assert(strcmp(out, "devbox") == 0);
   entity_name_normalize("My   Main   Box", out, sizeof(out));
   assert(strcmp(out, "my main box") == 0);
   entity_name_normalize("192.168.1.254", out, sizeof(out));
   assert(strcmp(out, "192.168.1.254") == 0); /* punctuation preserved */
   entity_name_normalize(NULL, out, sizeof(out));
   assert(out[0] == '\0');
   entity_name_normalize("", out, sizeof(out));
   assert(out[0] == '\0');
   /* The folds that were missing, and that the tier-A scorer had applied all
    * along -- so extraction measured cleaner in the benchmark than the graph it
    * produced actually was. Each of these used to be a SEPARATE entity node,
    * making every fact about one invisible to a query about another. */
   entity_name_normalize("kb_server", out, sizeof(out));
   assert(strcmp(out, "kb server") == 0);
   entity_name_normalize("kb-server", out, sizeof(out));
   assert(strcmp(out, "kb server") == 0);
   entity_name_normalize("KB server", out, sizeof(out));
   assert(strcmp(out, "kb server") == 0);
   entity_name_normalize("the Sunshine team", out, sizeof(out));
   assert(strcmp(out, "sunshine") == 0);
   entity_name_normalize("sunshine_team", out, sizeof(out));
   assert(strcmp(out, "sunshine") == 0);
   entity_name_normalize("Sunshine", out, sizeof(out));
   assert(strcmp(out, "sunshine") == 0);
   entity_name_normalize("Dr. Okafor", out, sizeof(out));
   assert(strcmp(out, "okafor") == 0);
   entity_name_normalize("Wellington.", out, sizeof(out));
   assert(strcmp(out, "wellington") == 0);

   /* And the folds it must NOT do. A missed fold leaves two nodes and an alias
    * can join them later; a WRONG fold welds two real entities together and
    * leaves no evidence to undo it from, so these matter more than the ones
    * above. Product names carry these words inside them. */
   entity_name_normalize("Girder Gateway van", out, sizeof(out));
   assert(strcmp(out, "girder gateway van") == 0);
   entity_name_normalize("Ingot Router", out, sizeof(out));
   assert(strcmp(out, "ingot router") == 0);
   entity_name_normalize("192.168.1.254", out, sizeof(out)); /* internal dots kept */
   assert(strcmp(out, "192.168.1.254") == 0);
   entity_name_normalize("example.com", out, sizeof(out));
   assert(strcmp(out, "example.com") == 0);
   /* Never strip a name away entirely. */
   entity_name_normalize("Team", out, sizeof(out));
   assert(strcmp(out, "team") == 0);
   entity_name_normalize("The", out, sizeof(out));
   assert(strcmp(out, "the") == 0);
   printf("  PASS: test_normalize\n");
}

int main(void)
{
   db2_test_shim_open();
   test_normalize();

   /* get-or-create + resolve */
   int64_t cid = db2_entity_register_named("DevBox", NODE_DEVICE);
   assert(cid > 0);
   assert(db2_entity_register_named("DevBox", NODE_DEVICE) == cid); /* idempotent */
   assert(db2_entity_resolve("devbox") == cid);                     /* normalized */
   assert(db2_entity_resolve("  DEVBOX ") == cid);
   /* End to end: the three spellings of one client reach ONE node. */
   int64_t sun = db2_entity_register_named("Sunshine", NODE_ORG);
   assert(sun > 0);
   assert(db2_entity_register_named("Sunshine team", NODE_ORG) == sun);
   assert(db2_entity_register_named("sunshine_team", NODE_ORG) == sun);
   assert(db2_entity_register_named("the Sunshine Team", NODE_ORG) == sun);
   assert(db2_entity_resolve("SUNSHINE") == sun);
   assert(db2_entity_kind(cid) == NODE_DEVICE);

   /* a second alias for the same entity resolves to the same canonical id */
   assert(db2_entity_alias_bind("the workstation", cid, 0) == 0);
   assert(db2_entity_resolve("The Workstation") == cid);

   /* first binding wins: binding an already-bound name to a different id is a
    * no-op (the name keeps resolving to the original entity). */
   int64_t other = db2_entity_register_named("acme corp", NODE_ORG);
   assert(other > 0 && other != cid);
   assert(db2_entity_alias_bind("DevBox", other, 0) == 0); /* ON CONFLICT DO NOTHING */
   assert(db2_entity_resolve("DevBox") == cid);            /* unchanged */

   /* aliases_for returns the bound names (preferred first). */
   char names[8][128];
   int n = db2_entity_aliases_for(cid, names, 8);
   assert(n == 2);
   assert(strcmp(names[0], "DevBox") == 0); /* is_preferred */

   /* unknown name resolves to 0 (not an error). */
   assert(db2_entity_resolve("never seen this") == 0);
   assert(db2_entity_kind(999999) == -1);

   /* merged_into is followed exactly one hop on resolve. */
   int64_t a = db2_entity_register_named("alpha box", NODE_DEVICE);
   int64_t b = db2_entity_register_named("beta box", NODE_DEVICE);
   int64_t cc = db2_entity_register_named("gamma box", NODE_DEVICE);
   assert(a > 0 && b > 0 && cc > 0);
   assert(db2_entity_mark_merged(a, b) == 0);
   assert(db2_entity_resolve("alpha box") == b); /* A -> B */
   assert(db2_entity_mark_merged(b, cc) == 0);
   assert(db2_entity_resolve("alpha box") == b); /* single hop: B, not C */
   assert(db2_entity_resolve("beta box") == cc); /* B -> C */
   assert(db2_entity_mark_merged(0, b) == -1);   /* bad args */
   assert(db2_entity_mark_merged(b, b) == -1);   /* self-merge rejected */

   /* NULL / empty / dangling input. */
   assert(db2_entity_register_named(NULL, NODE_DEVICE) == -1);
   assert(db2_entity_resolve("") == 0);
   assert(db2_entity_alias_bind("", cid, 1) == -1);
   assert(db2_entity_alias_bind("dangle", 999999, 1) == -1); /* target must exist */

   /* first-class merge / unmerge (reversible via the merged_into follow). */
   int64_t m_from = db2_entity_register_named("oldname box", NODE_DEVICE);
   int64_t m_into = db2_entity_register_named("newname box", NODE_DEVICE);
   assert(m_from > 0 && m_into > 0);
   int64_t mid = db2_entity_merge(m_from, m_into);
   assert(mid > 0);
   assert(db2_entity_resolve("oldname box") == m_into); /* merged -> follows */
   assert(db2_entity_unmerge(mid) == 0);
   assert(db2_entity_resolve("oldname box") == m_from); /* restored */
   assert(db2_entity_unmerge(mid) == -1);               /* already undone */
   assert(db2_entity_merge(m_from, m_from) == -1);      /* self-merge rejected */
   assert(db2_entity_merge(m_from, 999999) == -1);      /* missing target rejected */

   /* entity_name_conflicts queue. */
   int64_t conf = db2_entity_conflict_record("ambiguous theo");
   assert(conf > 0);
   assert(db2_entity_conflict_priority("ambiguous theo") == 1);
   assert(db2_entity_conflict_record("ambiguous theo") == conf); /* idempotent on name */
   assert(db2_entity_conflict_priority("ambiguous theo") == 2);  /* repeat bumps priority */
   assert(db2_entity_conflict_count("open") == 1);
   assert(db2_entity_conflict_set_status(conf, ENTITY_CONFLICT_RESOLVED) == 0);
   assert(db2_entity_conflict_count("open") == 0);
   assert(db2_entity_conflict_count(NULL) == 1);
   /* re-recording a resolved conflict bumps priority but does NOT reopen it. */
   assert(db2_entity_conflict_record("ambiguous theo") == conf);
   assert(db2_entity_conflict_priority("ambiguous theo") == 3);
   assert(db2_entity_conflict_count("open") == 0);
   assert(db2_entity_conflict_priority("never recorded") == -1);

   /* merge state machine: cycle, already-merged, and single-hop after a chain. */
   int64_t x = db2_entity_register_named("xenon box", NODE_DEVICE);
   int64_t y = db2_entity_register_named("yttrium box", NODE_DEVICE);
   int64_t z = db2_entity_register_named("zinc box", NODE_DEVICE);
   assert(x > 0 && y > 0 && z > 0);
   int64_t mxy = db2_entity_merge(x, y); /* X -> Y */
   assert(mxy > 0);
   assert(db2_entity_resolve("xenon box") == y);
   assert(db2_entity_merge(y, x) == -1);         /* cycle: target X no longer active */
   assert(db2_entity_resolve("xenon box") == y); /* X still merged into Y */
   assert(db2_entity_merge(x, z) == -1);         /* X already merged -> not active */
   int64_t myz = db2_entity_merge(y, z);         /* Y -> Z (Y still active) */
   assert(myz > 0);
   assert(db2_entity_resolve("xenon box") == y); /* single hop: Y, not Z */
   assert(db2_entity_resolve("yttrium box") == z);
   /* unmerging X->Y restores X even though Y is itself now merged. */
   assert(db2_entity_unmerge(mxy) == 0);
   assert(db2_entity_resolve("xenon box") == x);

   /* Migration: rows written before the normaliser learned its folds carry old
    * keys. Simulated by binding under an old-style key directly, then re-keying.
    * Without this pass the new folds are worse than the old behaviour -- the
    * lookup misses the stored row and mints a SECOND entity. */
   {
      int64_t legacy = db2_entity_register(NODE_ORG, ENTITY_STATUS_ACTIVE);
      assert(legacy > 0);
      char err[256] = "";
      aimee_pg_stmt_t *ins = aimee_pg_prepare(
          db2_conn(),
          "INSERT INTO entity_aliases (name, name_norm, canonical_id, is_preferred)"
          " VALUES (?1, ?2, ?3, 1)",
          err, sizeof(err));
      assert(ins);
      aimee_pg_bind_text(ins, "?1", "Halden_Freight");
      aimee_pg_bind_text(ins, "?2", "halden_freight"); /* OLD normaliser output */
      aimee_pg_bind_int64(ins, "?3", legacy);
      assert(aimee_pg_step(ins, err, sizeof(err)) != AIMEE_PG_ERR);
      aimee_pg_finalize(ins);

      /* Before the re-key the modern lookup cannot see it. */
      assert(db2_entity_resolve("Halden Freight") == 0);
      assert(db2_entity_renormalize_aliases() >= 1);
      /* After, all three spellings reach the row that already existed. */
      assert(db2_entity_resolve("Halden Freight") == legacy);
      assert(db2_entity_resolve("halden-freight") == legacy);
      assert(db2_entity_register_named("Halden Freight", NODE_ORG) == legacy);
      /* Idempotent: a second pass finds nothing to do. */
      assert(db2_entity_renormalize_aliases() == 0);
      printf("  PASS: legacy alias keys re-normalise instead of duplicating\n");
   }
   db2_test_shim_close();

   printf("entity_registry: all tests passed\n");
   return 0;
}
