/* gw_stage_memory.h: memory/context injection on the IR (ir_stage_memory), plus
 * the one adapter still used by the not-yet-ported plain-chat handlers.
 *
 * The per-wire gw_stage_memory() and its three render targets are DELETED: both
 * structured arms were ported to the IR seam, and keeping three hand-synchronised
 * copies of one policy is how the guidance text drifted in the first place.
 *
 * (historical header follows)
 * gw_stage_memory.h: the ONE memory-injection stage shared by every aimee
 * ingress (universal-gateway P3). Consolidates the formerly per-ingress memory
 * stages and the legacy inline ingress_preinject_build calls behind a single
 * stage that renders the <aimee-context> envelope per gw_request_t.mem_target. */
#ifndef DEC_GW_STAGE_MEMORY_H
#define DEC_GW_STAGE_MEMORY_H

#include <aimee/gateway/gateway_pipeline.h>
#include <aimee/ir/aimee_ir.h> /* aimee_request_t: the IR the transform edits */

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

   /* True when the model has not spoken yet in this conversation -- the opening
    * turn, and again after a compaction (a carried-over summary holds no assistant
    * turn). The one definition of "session start" shared by the stages below. */
   int ir_session_start(const aimee_request_t *ir);

   /* Withhold Codex's shell tools for the opening turn only, so the first look at
    * a tree goes through aimee's symbol-scoped tools instead of grep. Returns >0
    * when it removed something. apply_patch/update_plan are untouched, and from
    * the second turn the shell is back unconditionally. Naming the tools in the
    * guidance was measured NOT to be enough on its own. */
   int ir_stage_first_turn_shell_block(aimee_request_t *ir, void *ud);

   /* Adapter for the legacy OpenAI text handlers (/v1/chat/completions,
    * /v1/completions, and the buffered/streaming chat paths) that pass the
    * envelope to agent_execute() as the system prompt. Routes `query` through
    * ingress_preinject_build and returns the rendered
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
