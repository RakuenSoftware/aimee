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

   db2_test_shim_close();
   printf("entity_registry: all tests passed\n");
   return 0;
}
