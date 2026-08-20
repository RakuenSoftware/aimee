/* test_mcp_memory_gate.c: the authorization decisions behind the two MCP memory
 * tools that can destroy stored data.
 *
 * These exist because tool_memory_mutate's `forget` and `update` verbs reached a
 * hard DELETE and a no-history UPDATE with no capability check at all, and
 * `memory_maintain`'s prune mode bulk-deletes with no gate anywhere. Both gates
 * are one `if` away from being decorative, and no test links
 * server_mcp_call_table.c (it pulls in every tool aimee has), so the decisions
 * were extracted into a pure TU precisely so they could be pinned here. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aimee.h"
#include "memory.h" /* MEMORY_MAINTENANCE_MODE_* — not standalone; needs aimee.h first */
#include "server.h" /* CAP_* bits, server_capability_for_method */
#include "server_mcp_memory_gate.h"

/* Every mutate verb maps to the RPC method that does the same thing, so the MCP
 * door inherits the grade the NDJSON/HTTP door already had. `forget` is the one
 * that matters: it must land on memory.delete, whose capability is deliberately
 * NOT the one memory.store carries. */
static void test_verb_methods(void)
{
   assert(strcmp(mcp_mutate_verb_method("store"), "memory.store") == 0);
   assert(strcmp(mcp_mutate_verb_method("update"), "memory.update") == 0);
   assert(strcmp(mcp_mutate_verb_method("supersede"), "memory.supersede") == 0);
   assert(strcmp(mcp_mutate_verb_method("forget"), "memory.delete") == 0);
   assert(strcmp(mcp_mutate_verb_method("affirm"), "memory.touch") == 0);
   assert(strcmp(mcp_mutate_verb_method("reject"), "memory.reject") == 0);

   /* Unknown and NULL verbs return NULL so the caller falls through to
    * tool_memory_mutate's own "unknown verb" error rather than being gated on a
    * method nobody named. */
   assert(mcp_mutate_verb_method("wipe") == NULL);
   assert(mcp_mutate_verb_method("") == NULL);
   assert(mcp_mutate_verb_method(NULL) == NULL);
   printf("  PASS: mutate verbs map to their RPC method twins\n");
}

/* The mapping is only worth anything if the methods it names still grade the way
 * the fix intended -- a verb correctly routed to memory.delete buys nothing if
 * memory.delete drifts back to CAP_MEMORY_WRITE. Pin the composition. */
static void test_verb_grades_are_what_the_fix_intended(void)
{
   assert(server_capability_for_method(mcp_mutate_verb_method("forget")) == CAP_MEMORY_ADMIN);
   assert(server_capability_for_method(mcp_mutate_verb_method("store")) == CAP_MEMORY_WRITE);
   assert(server_capability_for_method(mcp_mutate_verb_method("update")) == CAP_MEMORY_WRITE);

   /* Destroying and writing must not be the same grant. */
   assert(server_capability_for_method(mcp_mutate_verb_method("forget")) !=
          server_capability_for_method(mcp_mutate_verb_method("store")));

   /* update must not have fallen back to the memory.* read prefix, which is
    * where it sat before -- a content overwrite gated as a read. */
   assert(server_capability_for_method(mcp_mutate_verb_method("update")) != CAP_MEMORY_READ);
   printf("  PASS: forget grades as memory:admin, store/update as memory:write\n");
}

/* memory_maintain's prune mode hard-deletes: every L0 row and its provenance,
 * stale L1 rows, and restricted/sensitive memories past retention. */
static void test_maintain_prune_requires_admin(void)
{
   assert(mcp_memory_maintain_required_cap(MEMORY_MAINTENANCE_MODE_PRUNE) == CAP_MEMORY_ADMIN);

   /* Prune combined with harmless modes is still destructive. */
   assert(mcp_memory_maintain_required_cap(MEMORY_MAINTENANCE_MODE_REPLAY |
                                           MEMORY_MAINTENANCE_MODE_PRUNE) == CAP_MEMORY_ADMIN);
   printf("  PASS: prune requires memory:admin\n");
}

/* The subtle half: modes of 0 means MEMORY_MAINTENANCE_MODES_DEFAULT, which
 * includes prune. A bare `memory_maintain {}` therefore DELETES, and must grade
 * as destructive. This is the case a reader most easily misses, and the one that
 * would silently drop to memory:write if prune ever left the default set. */
static void test_maintain_default_modes_require_admin(void)
{
   assert((MEMORY_MAINTENANCE_MODES_DEFAULT & MEMORY_MAINTENANCE_MODE_PRUNE) != 0);
   assert(mcp_memory_maintain_required_cap(0) == CAP_MEMORY_ADMIN);
   assert(mcp_memory_maintain_required_cap(0) ==
          mcp_memory_maintain_required_cap(MEMORY_MAINTENANCE_MODE_PRUNE));
   printf("  PASS: a bare memory_maintain {} grades as destructive\n");
}

/* Non-destructive maintenance is a write, not an admin action -- the gate should
 * not be so blunt that ordinary upkeep needs the destroy grant. */
static void test_maintain_non_destructive_modes_require_write(void)
{
   assert(mcp_memory_maintain_required_cap(MEMORY_MAINTENANCE_MODE_REPLAY) == CAP_MEMORY_WRITE);
   assert(mcp_memory_maintain_required_cap(MEMORY_MAINTENANCE_MODE_COMPACT) == CAP_MEMORY_WRITE);
   assert(mcp_memory_maintain_required_cap(MEMORY_MAINTENANCE_MODE_SUMMARIZE) == CAP_MEMORY_WRITE);
   assert(mcp_memory_maintain_required_cap(MEMORY_MAINTENANCE_MODE_DRIFT) == CAP_MEMORY_WRITE);
   assert(mcp_memory_maintain_required_cap(MEMORY_MAINTENANCE_MODE_REPLAY |
                                           MEMORY_MAINTENANCE_MODE_COMPACT) == CAP_MEMORY_WRITE);
   printf("  PASS: non-destructive modes require only memory:write\n");
}

/* Neither gate may be satisfiable by a read-only caller. */
static void test_no_gate_is_read_only(void)
{
   assert((CAPS_READ_ONLY & CAP_MEMORY_ADMIN) == 0);
   assert((CAPS_READ_ONLY & CAP_MEMORY_WRITE) == 0);
   assert((CAPS_READ_ONLY & mcp_memory_maintain_required_cap(0)) == 0);
   assert((CAPS_READ_ONLY & server_capability_for_method(mcp_mutate_verb_method("forget"))) == 0);
   printf("  PASS: no memory gate is satisfied by a read-only caller\n");
}

int main(void)
{
   printf("mcp_memory_gate:\n");
   test_verb_methods();
   test_verb_grades_are_what_the_fix_intended();
   test_maintain_prune_requires_admin();
   test_maintain_default_modes_require_admin();
   test_maintain_non_destructive_modes_require_write();
   test_no_gate_is_read_only();
   printf("mcp_memory_gate: all tests passed\n");
   return 0;
}
