/* wfe_deliver.h -- gate.deliver runtime re-verification (I3, fork Q4).
 *
 * Reaching gate.deliver should already imply that the upstream review +
 * roundtable passed (by the I2 structure + each gate's on_pass edge). Q4 adds a
 * cheap, structural defense-in-depth check against a future wiring bug, a
 * hand-edited workflow, or an I2 validator regression: before gate.deliver
 * advances, re-verify that every *delivery-gating* gate has an approving record.
 *
 * This is expressed as a POLICY OVER THE VERDICT GRAPH, not a hard-coded
 * "review + roundtable" allowlist (so inserting a gate later cannot silently
 * bypass it): a gate node is delivery-gating iff it is a verdict/approval gate
 * that is reachable from start AND from whose SUCCESS edges (on_pass/next) the
 * deliver node is reachable. The presence/status lookup is abstracted behind a
 * callback so the check is a pure STRUCTURAL lookup (never an LLM re-judgement)
 * and reads engine-owned, append-only records -- the caller supplies a predicate
 * that consults them. On any missing/non-approving gate the caller must HALT the
 * run (terminal ERROR), never loop.
 *
 * Design per the I1/I3 roundtable consult (2026-07-01), fork Q4 #8/#9/#28/#29/#31.
 */
#ifndef DEC_WFE_DELIVER_H
#define DEC_WFE_DELIVER_H 1

#include <stddef.h>

#include "wfe_def.h"

/* Predicate: has the gate node `node_id` recorded an approving verdict/approval
 * for this run? Must read engine-owned append-only state (lifecycle/verdict
 * records), NEVER delegate-writable state. Returns 1 = approved, 0 = not. */
typedef int (*wfe_gate_advanced_fn)(const char *node_id, void *ctx);

/* Re-verify the delivery gate `deliver_id` in `def`. Returns 0 iff every
 * delivery-gating gate satisfies `advanced`; -1 (with `err` set to the first
 * offending gate id) otherwise. Bad args (NULL def/deliver_id/advanced, or
 * deliver_id not a node) return -1. */
int wfe_deliver_reverify(const wfe_def_t *def, const char *deliver_id,
                         wfe_gate_advanced_fn advanced, void *ctx, char *err, size_t errlen);

/* 1 if `t` is a verdict/approval gate (roundtable, human, review, ci,
 * mergeable) -- the block kinds whose approval gates delivery. */
int wfe_block_is_verdict_gate(wfe_block_type_t t);

#endif /* DEC_WFE_DELIVER_H */
