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

/* The gate alone cannot stop a model from bulk-deleting, and this is the reason:
 * reaching ANY MCP tool needs CAP_TOOL_EXECUTE, which exists only in
 * CAPS_AUTHENTICATED and CAPS_ALL -- and both of those also carry
 * CAP_MEMORY_ADMIN. So every caller that can invoke memory_maintain at all
 * already clears an admin-graded gate. Pin the property, because it is the
 * justification for stripping prune rather than merely grading it: if the
 * capability sets are ever separated, that is a deliberate change and this
 * assertion should be revisited, not silently invalidated. */
static void test_every_mcp_caller_already_clears_the_admin_gate(void)
{
   uint32_t mcp = server_capability_for_method("mcp.call");
   assert(mcp == CAP_TOOL_EXECUTE);

   /* The only cap sets a connection can hold are CAPS_ALL, CAPS_AUTHENTICATED,
    * and the read-only set (see server_http_conn_caps). */
   assert((CAPS_READ_ONLY & CAP_TOOL_EXECUTE) == 0); /* read-only cannot reach MCP at all */
   assert((CAPS_AUTHENTICATED & CAP_TOOL_EXECUTE) == CAP_TOOL_EXECUTE);
   assert((CAPS_ALL & CAP_TOOL_EXECUTE) == CAP_TOOL_EXECUTE);
   /* ...and both MCP-capable sets already hold the destroy grant. */
   assert((CAPS_AUTHENTICATED & CAP_MEMORY_ADMIN) == CAP_MEMORY_ADMIN);
   assert((CAPS_ALL & CAP_MEMORY_ADMIN) == CAP_MEMORY_ADMIN);
   printf("  PASS: every MCP-capable caller already holds memory:admin\n");
}

/* Which is why the model's door does not prune at all. */
static void test_model_modes_never_prune(void)
{
   int dropped = -1;

   /* Explicit prune is removed, and reported. */
   unsigned int m = mcp_memory_maintain_model_modes(MEMORY_MAINTENANCE_MODE_PRUNE, &dropped);
   assert((m & MEMORY_MAINTENANCE_MODE_PRUNE) == 0);
   assert(m == 0); /* nothing else was asked for */
   assert(dropped == 1);

   /* A bare call -- modes 0 -> MODES_DEFAULT, which includes prune -- loses only
    * prune and still runs the rest. This is the case that silently destroyed. */
   dropped = -1;
   m = mcp_memory_maintain_model_modes(0, &dropped);
   assert((m & MEMORY_MAINTENANCE_MODE_PRUNE) == 0);
   assert(dropped == 1);
   assert((m & MEMORY_MAINTENANCE_MODE_REPLAY) == MEMORY_MAINTENANCE_MODE_REPLAY);
   assert((m & MEMORY_MAINTENANCE_MODE_COMPACT) == MEMORY_MAINTENANCE_MODE_COMPACT);

   /* Prune mixed with real work keeps the work. */
   dropped = -1;
   m = mcp_memory_maintain_model_modes(
       MEMORY_MAINTENANCE_MODE_PRUNE | MEMORY_MAINTENANCE_MODE_SUMMARIZE, &dropped);
   assert(m == MEMORY_MAINTENANCE_MODE_SUMMARIZE);
   assert(dropped == 1);

   /* A request with no prune in it is passed through untouched and not reported
    * as narrowed. */
   dropped = -1;
   m = mcp_memory_maintain_model_modes(MEMORY_MAINTENANCE_MODE_REPLAY, &dropped);
   assert(m == MEMORY_MAINTENANCE_MODE_REPLAY);
   assert(dropped == 0);

   /* Whatever survives is never admin-graded, since prune is what made it so. */
   assert(mcp_memory_maintain_required_cap(mcp_memory_maintain_model_modes(0, NULL)) ==
          CAP_MEMORY_WRITE);
   printf("  PASS: the model's maintenance door never prunes\n");
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

/* Capability answers "may they", attestation answers "who are they", and the two
 * memory verbs that cannot be undone need the second question asked as well.
 *
 * CAP_MEMORY_ADMIN sits inside CAPS_AUTHENTICATED, so a bearer clears it — over
 * TCP too, under remote_writes=DATA/FULL. That is the right answer for "may this
 * caller delete", and the wrong one for "is this caller the user", which is what
 * decides whether memory.delete destroys the row or retires it and whether a
 * stored note may later mint Class-A facts. Only the two kernel/root-attested
 * local shapes are a person. */
static void test_attested_identity_is_not_capability(void)
{
   assert(server_attested_is_person(ATTEST_UDS_PEERCRED) == 1);
   assert(server_attested_is_person(ATTEST_WEBCHAT_TRUSTED) == 1);

   /* A shared token is a service or an agent, however much it may do. */
   assert(server_attested_is_person(ATTEST_TCP_BEARER) == 0);
   assert(server_attested_is_person(ATTEST_TLS_BEARER) == 0);
   assert(server_attested_is_person(ATTEST_MTLS_CLIENT) == 0);

   /* The zero value: a missed hop must never become a user. */
   assert(server_attested_is_person(ATTEST_NONE) == 0);

   assert(server_attested_memory_authority(ATTEST_UDS_PEERCRED) == MEMORY_AUTHORITY_USER);
   assert(server_attested_memory_authority(ATTEST_WEBCHAT_TRUSTED) == MEMORY_AUTHORITY_USER);
   assert(server_attested_memory_authority(ATTEST_TCP_BEARER) == MEMORY_AUTHORITY_MODEL);
   assert(server_attested_memory_authority(ATTEST_MTLS_CLIENT) == MEMORY_AUTHORITY_MODEL);
   assert(server_attested_memory_authority(ATTEST_NONE) == MEMORY_AUTHORITY_MODEL);

   /* The point of the split: a caller can hold the destructive capability and
    * still not be a person, which is exactly the case the mapping must catch. */
   assert((CAPS_AUTHENTICATED & CAP_MEMORY_ADMIN) != 0);
   assert(server_attested_is_person(ATTEST_TCP_BEARER) == 0);
   printf("  PASS: attested identity is asked separately from capability\n");
}

int main(void)
{
   printf("mcp_memory_gate:\n");
   test_attested_identity_is_not_capability();
   test_verb_methods();
   test_verb_grades_are_what_the_fix_intended();
   test_maintain_prune_requires_admin();
   test_maintain_default_modes_require_admin();
   test_maintain_non_destructive_modes_require_write();
   test_every_mcp_caller_already_clears_the_admin_gate();
   test_model_modes_never_prune();
   test_no_gate_is_read_only();
   printf("mcp_memory_gate: all tests passed\n");
   return 0;
}
