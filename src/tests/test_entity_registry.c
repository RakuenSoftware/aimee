/* test_entity_registry.c: surrogate-id entity canonicalization (typed-fact §3 /
 * P2a), against the sqlite shim. */
#include "../headers/aimee.h"
#include "../db2/entity_registry.h"
#include "../db2/db2_test_shim.h"
#include "../headers/memory_ontology.h"
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
   assert(db2_entity_conflict_record("ambiguous theo") == conf); /* idempotent on name */
   assert(db2_entity_conflict_count("open") == 1);
   assert(db2_entity_conflict_set_status(conf, ENTITY_CONFLICT_RESOLVED) == 0);
   assert(db2_entity_conflict_count("open") == 0);
   assert(db2_entity_conflict_count(NULL) == 1);

   db2_test_shim_close();
   printf("entity_registry: all tests passed\n");
   return 0;
}
