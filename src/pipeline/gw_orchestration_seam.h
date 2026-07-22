/* gw_orchestration_seam.h -- the ORCHESTRATION-HOOK seam: where delegates and workflows
 * observe/act on a turn. Slice 3 of the response/orchestration-stages proposal.
 *
 * DISTINCT from the request/response stage registries. A request/response STAGE transforms
 * a payload (mutates raw cJSON / parsed_response and returns an intervention count). An
 * orchestration HOOK acts on the TURN: it may spawn a delegate, advance a workflow,
 * short-circuit the turn, or suspend it for later re-entry. Conflating the two interfaces
 * was explicitly ruled against by the roundtable, so this seam has its own types.
 *
 * Roundtable-ruled contract (2026-07-17):
 *   - an IMMUTABLE turn snapshot (the hook observes, it does not rewrite the request/reply),
 *   - NARROW capability handles (the hook spawns/dispatches through provided handles, it does
 *     not reach into globals),
 *   - a TAGGED result: continue / complete(short-circuit) / suspend(with continuation) / fail.
 *   - Fail-OPEN, NOT fail-closed: a misconfigured or failing orchestration hook must NOT block
 *     a turn from completing (the opposite of the governance response stage, which must never
 *     be silently skipped). The runner keeps running later hooks after a FAIL but REMEMBERS it
 *     and surfaces an aggregate FAIL, so the wire site can log it (this core stays I/O-free).
 */
#ifndef DEC_GW_ORCHESTRATION_SEAM_H
#define DEC_GW_ORCHESTRATION_SEAM_H 1

#include <stddef.h>

struct cJSON;
struct parsed_response;

/* Immutable view of the turn handed to every hook. All pointers are BORROWED and read-only
 * for the hook's lifetime; a hook that needs to mutate does so through the capability
 * handles, never through this snapshot. Grows by adding fields, never a bare pointer. */
typedef struct
{
   const char *turn_id;                    /* correlation id for this turn (never NULL) */
   const char *session_id;                 /* originating session id, or NULL if none */
   const struct cJSON *request;            /* inbound request payload, read-only */
   const struct parsed_response *response; /* reply so far, or NULL before the response exists */
} gw_turn_snapshot_t;

/* Narrow capability handles: the ONLY way a hook effects change. Each returns 0 when the
 * action was accepted, <0 on refusal. `ctx` is the opaque backing the seam installs; a hook
 * passes it back unchanged. Kept minimal on purpose -- the first port needs exactly these two
 * verbs; a new verb is a new field with its own justification, not a general escape hatch. */
typedef struct
{
   void *ctx;
   int (*spawn_delegate)(void *ctx, const char *role, const char *brief);
   int (*dispatch_workflow)(void *ctx, const char *lane, const char *payload);
} gw_turn_capabilities_t;

/* A hook's outcome. Unlike the response stage's ok/reject/error (fail-closed), these are
 * turn-control verbs (fail-OPEN):
 *   CONTINUE  -- proceed to the next hook / with the turn as normal.
 *   COMPLETE  -- the hook fully handled the turn; stop running later hooks, short-circuit.
 *   SUSPEND   -- pause the turn; `continuation` names how to re-enter (see ownership below).
 *   FAIL      -- the hook failed; logged, the turn STILL completes (fail-open). */
typedef enum
{
   GW_ORCH_CONTINUE = 0,
   GW_ORCH_COMPLETE,
   GW_ORCH_SUSPEND,
   GW_ORCH_FAIL
} gw_orch_status_t;

typedef struct
{
   gw_orch_status_t status;
   /* SUSPEND only, else NULL. OWNERSHIP: a heap string the hook allocates and TRANSFERS up
    * through the runner to the CALLER, which frees it after using it to schedule re-entry.
    * The hook must never free it; the runner never frees it (it only propagates the SUSPEND
    * result). Idempotency + cancellation of a suspended turn are the resumption sub-slice's
    * concern, not this registry's. */
   char *continuation;
} gw_orch_result_t;

/* A single orchestration hook: observe `turn`, act through `caps`, return a control verb. */
typedef gw_orch_result_t (*gw_orch_hook_fn)(const gw_turn_snapshot_t *turn,
                                            const gw_turn_capabilities_t *caps, void *ud);

typedef struct
{
   gw_orch_hook_fn fn;
   void *ud;
   const char *name;
} gw_orch_hook_t;

/* One candidate hook in an ordered catalog; enabled==0 removes the module (delegates or
 * workflows register here, NOT in the request/response stage registries). */
typedef struct
{
   const char *name;
   gw_orch_hook_fn fn;
   void *ud;
   int enabled;
} gw_orch_hook_slot_t;

/* Build the enabled, ordered hook array from `slots` into `out` (cap). Returns the count
 * (>=0), or -1 on a hard catalog error: duplicate enabled name, enabled slot with empty
 * name / NULL fn, or output overflow. Mirrors gw_response_registry_build -- catalog
 * construction is validated up front. */
int gw_orchestration_registry_build(const gw_orch_hook_slot_t *slots, size_t n_slots,
                                    gw_orch_hook_t *out, size_t cap);

/* Run `hooks` over the turn in order, fail-OPEN. Semantics:
 *   - CONTINUE  -> run the next hook.
 *   - COMPLETE  -> stop; return COMPLETE (the caller short-circuits the turn).
 *   - SUSPEND   -> stop; return SUSPEND, propagating the hook's continuation to the caller
 *                  (the caller owns and frees it; see gw_orch_result_t). A SUSPEND with a NULL
 *                  continuation is malformed and handled as a fail-open failure instead.
 *   - FAIL / unknown status / NULL hook fn / malformed SUSPEND -> the turn is NEVER blocked
 *                  (fail-open), but the failure is REMEMBERED: if no COMPLETE/SUSPEND supersedes
 *                  it, the aggregate result is FAIL so the wire site can observe and log it.
 *                  Aggregate FAIL still means "the turn proceeds".
 * A NULL/empty hook list returns a clean CONTINUE. A NULL `turn`/`caps` returns FAIL (the
 * seam cannot act on a missing turn) WITHOUT aborting -- fail-open at the boundary too. */
gw_orch_result_t gw_orchestration_run(const gw_turn_snapshot_t *turn,
                                      const gw_turn_capabilities_t *caps,
                                      const gw_orch_hook_t *hooks, size_t n);

#endif /* DEC_GW_ORCHESTRATION_SEAM_H */
