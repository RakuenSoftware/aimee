/* gw_stage_memory.h: the ONE memory-injection stage shared by every aimee
 * ingress (universal-gateway P3). Consolidates the formerly per-ingress memory
 * stages and the legacy inline ingress_preinject_build calls behind a single
 * stage that renders the <aimee-context> envelope per gw_request_t.mem_target. */
#ifndef DEC_GW_STAGE_MEMORY_H
#define DEC_GW_STAGE_MEMORY_H

#include "gateway_pipeline.h"
#include "aimee_ir.h" /* aimee_request_t: the IR the transform edits */

#ifdef __cplusplus
extern "C"
{
#endif

   /* The IR-native memory transform (universal-gateway P4 / IR-canonical funnel):
    * the protocol-neutral replacement for gw_stage_memory's three per-wire arms.
    * Fires ONCE at the single request-transform seam (aimee_ir_apply_request_stages),
    * after frontend_parse and before backend_build, so it acts on the typed IR
    * regardless of client wire. Derives the recall query from the IR's last user
    * message (aimee_ir_last_user_text), builds the <aimee-context> envelope, and
    * appends it as a trailing system TEXT block on `ir->system` — cache-safe (the
    * cached prefix blocks are untouched; the new block carries no cache_control and
    * no raw sidecar, so a byte-faithful backend re-serializes it from the typed
    * field). `ud` is unused (the query comes from the IR). Conforms to
    * aimee_ir_transform_fn: returns 1 iff it appended a block (so the runner marks
    * ir->mutated and the stale raw sidecar is dropped), 0 on off/empty/no-query.
    * The same-protocol Anthropic passthrough never reaches this seam (it ships the
    * raw sidecar directly), so Arm A's parity-skip is structurally satisfied here. */
   int ir_stage_memory(aimee_request_t *ir, void *ud);

   /* The shared memory stage. Renders the envelope into `r->raw` per
    * `r->mem_target`. The `ud` (stage user data) contract is per target:
    *   - GW_MEM_ANTHROPIC_MESSAGES: `ud` unused — the query is derived inside
    *     messages_apply_preinject from `r->raw.messages`. Parity-gated.
    *   - GW_MEM_OPENAI_INSTRUCTIONS: `ud` is the chat-shape `messages` cJSON
    *     array the recall query is derived from. Merges env+"\n\n"+prior into
    *     `r->raw.instructions`.
    *   - GW_MEM_OPENAI_SYSTEM_PROMPT: `ud` is the recall query as a
    *     `const char *` (the legacy handler's transcript verbatim). Sets
    *     `r->raw.instructions` to the RAW env (no apply, no trailing newlines).
    * Returns >0 if it injected the envelope, 0 otherwise (off/empty/parity). */
   int gw_stage_memory(gw_request_t *r, void *ud);

   /* The Anthropic arm of the stage, exposed so the /v1/messages ingress tests
    * can exercise it directly: build the <aimee-context> envelope from
    * `req.messages` and append it as a trailing `system` text block (cache-safe).
    * No-op when pre-injection is off or recall is empty. */
   void messages_apply_preinject(struct cJSON *req);

   /* Adapter for the legacy OpenAI text handlers (/v1/chat/completions,
    * /v1/completions, and the buffered/streaming chat paths) that pass the
    * envelope to agent_execute() as the system prompt. Routes `query` through
    * gw_stage_memory with GW_MEM_OPENAI_SYSTEM_PROMPT and returns the rendered
    * system prompt (malloc'd, caller frees), or NULL when pre-injection is
    * off/empty — byte-for-byte what `ingress_preinject_build(query, 0)` returned
    * inline before P3. This is the ONLY sanctioned way for those handlers to
    * obtain the envelope; they must not call ingress_preinject_build directly
    * (keeping the build in one place is what makes the consolidation byte-safe). */
   char *gw_memory_system_prompt(const char *query);

   /* Slice 7: 1 unless AIMEE_STAGE_MEMORY is explicitly disabled (0/off/false). Lets
    * the memory injection stage be removed from the pipeline "at will" via config; the
    * registry omits the stage when this returns 0. Default-ON, matching pre-registry. */
   int gw_stage_memory_enabled(void);

#ifdef __cplusplus
}
#endif
#endif /* DEC_GW_STAGE_MEMORY_H */
