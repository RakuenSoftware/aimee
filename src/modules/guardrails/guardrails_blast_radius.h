/* guardrails_blast_radius.h: structural blast-radius ADVISORY for edits (§7).
 *
 * Proposal "code-graph intelligence" §7 (agent actuation) with the R1 safety
 * constraint: anything on the guardrail path uses ONLY the deterministic
 * structural layers (call graph + typed projection edges via the KB sidecar's
 * /v1/code/blast-radius), never the LLM-synthesised entity graph; the actuation
 * is ADVISORY and FAIL-OPEN — it surfaces context, never blocks, and a
 * missing/empty graph adds no restriction. The graph can only ADD caution.
 *
 * Split for testability: blast_radius_advisory_format() is a PURE function over
 * an already-fetched blast_radius_t (the testable gate core); the I/O glue that
 * resolves the project and queries the sidecar is kept thin and separate. */
#ifndef GUARDRAILS_BLAST_RADIUS_H
#define GUARDRAILS_BLAST_RADIUS_H

#include <stddef.h>

#include "index.h" /* blast_radius_t */

/* At/above this dependent count the edited symbol is treated as a high-centrality
 * hub and the advisory appends a refactor-risk note (matches classify_path's
 * SEV_RED threshold in guardrails.c). */
#define BR_ADVISORY_HUB_THRESHOLD 5
/* Cap the number of dependent file names listed in the advisory string. */
#define BR_ADVISORY_MAX_NAMES 8

/* PURE. Format a structural blast-radius advisory for an edit to `edited_path`
 * from its already-fetched structural dependents. Writes an "ADVISORY: ..."
 * string into msg_buf and returns 1 when an advisory is warranted (>=1
 * dependent); otherwise leaves msg_buf untouched and returns 0. Lists up to
 * BR_ADVISORY_MAX_NAMES dependents (then an ellipsis), and appends a hub note
 * at/above hub_threshold. Never blocks; truncation-safe. */
int blast_radius_advisory_format(const blast_radius_t *br, const char *edited_path,
                                 int hub_threshold, char *msg_buf, size_t msg_len);

/* I/O glue (FAIL-OPEN). Resolve `abs_path` to (project, relpath) over the
 * indexed project list and fetch its structural blast radius from the KB
 * sidecar. Returns 0 with *out filled on a project match + successful fetch,
 * -1 otherwise (caller skips the advisory on -1). */
int guardrails_blast_radius_for_abs_path(const char *abs_path, blast_radius_t *out);

/* Orchestrator hook (single call site, FAIL-OPEN). When the
 * `guardrails_blast_radius_advisory_enabled` config flag is on and msg_buf is
 * still empty (no higher-priority guardrail message), fetch + format a
 * structural blast-radius advisory for an edit to abs_path. Any error — flag
 * off, no project match, sidecar failure, no dependents — leaves msg_buf
 * untouched. Never blocks. */
void guardrails_blast_radius_advisory(const char *abs_path, char *msg_buf, size_t msg_len);

#endif /* GUARDRAILS_BLAST_RADIUS_H */
