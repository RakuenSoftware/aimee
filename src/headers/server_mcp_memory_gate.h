#ifndef SERVER_MCP_MEMORY_GATE_H
#define SERVER_MCP_MEMORY_GATE_H
/* Authorization decisions for the two MCP memory tools that can destroy stored
 * data: `mutate` (verbs store/update/supersede/forget/affirm/reject) and
 * `memory_maintain` (whose prune mode bulk-deletes).
 *
 * These live apart from server_mcp_call_table.c, and behind a header with no
 * heavy includes, for one reason: no test links the call table (it pulls in
 * every tool aimee has), so while the decisions lived there they were verified
 * by reading them and nothing else. They are the security-critical half of two
 * gates that exist because a model could otherwise permanently destroy a user's
 * memories. They should be pinned by tests.
 *
 * Both functions are pure -- no I/O, no globals, no capability lookup -- so the
 * implementation TU links with essentially nothing. */
#include <stdint.h>

/* The RPC method a `mutate` verb is equivalent to, for grading it through the
 * same capability table as the NDJSON/HTTP door (server_capability_for_method).
 * Returns NULL for an unknown/NULL verb; the caller lets tool_memory_mutate
 * report the bad verb rather than inventing a second error for it. */
const char *mcp_mutate_verb_method(const char *verb);

/* The capability `memory_maintain` requires for a given `modes` bitmask.
 *
 * CAP_MEMORY_ADMIN when the request includes prune -- which hard-deletes: it
 * wipes every L0 row and its provenance, deletes stale L1 rows, and drops
 * restricted/sensitive memories past their retention window. CAP_MEMORY_WRITE
 * otherwise.
 *
 * `modes` of 0 means MEMORY_MAINTENANCE_MODES_DEFAULT, which INCLUDES prune, so
 * a bare `memory_maintain {}` grades as destructive. Deliberately takes no
 * dry_run: that flag is set by the same caller, so letting it lower the grade
 * would move the bypass rather than close it. */
uint32_t mcp_memory_maintain_required_cap(unsigned int modes);

/* The maintenance modes a MODEL-initiated call may actually run: the requested
 * set (or MODES_DEFAULT when `modes` is 0) with prune removed. When
 * `dropped_prune_out` is non-NULL it reports whether prune was taken out, so the
 * caller can say so rather than silently doing less than asked.
 *
 * Why the capability gate alone is not enough. Reaching ANY MCP tool requires
 * CAP_TOOL_EXECUTE, which exists only in CAPS_AUTHENTICATED and CAPS_ALL -- and
 * both of those also contain CAP_MEMORY_ADMIN. So every caller that can invoke
 * memory_maintain at all already clears an admin-graded gate, and grading alone
 * would leave a model able to bulk-delete: prune wipes every L0 row and its
 * provenance, deletes stale L1 rows, and drops restricted/sensitive memories
 * past retention.
 *
 * So the model's door does not prune, for the same reason its `forget` retires
 * rather than destroys. Pruning stays available to the operator, who has a
 * separate path to it (`aimee memory maintain`, which reaches the knowledge
 * service directly), and to the scheduler that runs the cycle internally. */
unsigned int mcp_memory_maintain_model_modes(unsigned int modes, int *dropped_prune_out);

#endif /* SERVER_MCP_MEMORY_GATE_H */
