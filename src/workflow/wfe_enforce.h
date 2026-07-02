/* wfe_enforce.h -- S2 pure enforcement policy cores: the per-block tool surface
 * (which tools a bound session may use while a workflow block is current) and
 * the rollout fail-class split (instrumentation-failure vs policy-decision). No
 * engine / DB / gateway deps, so the decisions are unit-testable in isolation;
 * the binding lookup, the ingress snapshot, and the actual tool stripping are
 * integration layers on top. Design per the S2 roundtable consult (2026-07-01). */
#ifndef DEC_WFE_ENFORCE_H
#define DEC_WFE_ENFORCE_H 1

#include "wfe_iface.h" /* wfe_block_type_t */

/* The tool surface a bound session gets while a block is current; each level is
 * strictly broader than the previous. */
typedef enum
{
   WFE_SURFACE_READONLY = 0, /* read/search only: no write/edit/patch, no delegate,
                                no deliver primitive. understand/split/review/gates,
                                converse/research. */
   WFE_SURFACE_DELEGATE,     /* + delegate-launch; still no direct write / deliver.
                                implement/document -- the primary delegates, it does
                                not write the tree itself. */
   WFE_SURFACE_FULL          /* unrestricted, EXCEPT the deliver gate still applies
                                pre-delivery. non-enforced / unknown blocks. */
} wfe_tool_surface_t;

/* Default surface for a block type. A bound enforced session uses this unless the
 * block's YAML declares an explicit allowed-tool list (S2 integration layer). */
wfe_tool_surface_t wfe_block_default_surface(wfe_block_type_t t);

/* 1 if `tool_name` may be used under `surface` given the run's `delivered` state.
 * A deliver primitive is denied until delivered at EVERY surface; a write tool is
 * denied below FULL; a delegate-launch tool is denied under READONLY. NULL tool
 * -> denied (fail closed). */
int wfe_surface_allows(wfe_tool_surface_t surface, const char *tool_name, int delivered);

/* 1 if `tool_name` mutates the working tree (write/edit/patch/apply/notebook). */
int wfe_is_write_tool(const char *tool_name);
/* 1 if `tool_name` launches a delegate / sub-agent. */
int wfe_is_delegate_tool(const char *tool_name);

/* ---- rollout fail-class split (consult Q5) ---- */
typedef enum
{
   WFE_FAIL_INSTRUMENTATION = 0, /* could not DETERMINE policy: binding lookup threw,
                                    DB unreachable, eval error. */
   WFE_FAIL_POLICY               /* policy DECIDED no (gate not passed / tool denied) --
                                    NOT an error. */
} wfe_fail_class_t;

typedef enum
{
   WFE_ACT_FAIL_OPEN_CHAT = 0, /* allow the turn as normal unbound chat (+ loud log) */
   WFE_ACT_FAIL_CLOSED         /* refuse the action */
} wfe_fail_action_t;

/* Decide what to do on a failure. `hard` = the enforcement dial is at hard.
 * `is_deliver_or_write` = the action being decided is a delivery/externalization/
 * write primitive (vs plain chat text). A POLICY denial ALWAYS fails closed. An
 * INSTRUMENTATION failure fails open to preserve chat, but fails CLOSED for a
 * deliver/write primitive when the dial is hard (so a lookup error cannot bypass
 * delivery enforcement). */
wfe_fail_action_t wfe_enforce_fail_action(wfe_fail_class_t cls, int hard, int is_deliver_or_write);

/* ---- the staged rollout dial (consult Q5; default OFF) ---- */
typedef enum
{
   WFE_ENFORCE_OFF = 0,  /* no binding, no enforcement (default) */
   WFE_ENFORCE_ADVISORY, /* bind + log the decision; do NOT restrict tools/deliver */
   WFE_ENFORCE_SOFT,     /* warn the user + primary on a denied action, but ALLOW it */
   WFE_ENFORCE_HARD      /* refuse a denied action */
} wfe_enforce_stage_t;

wfe_enforce_stage_t wfe_enforce_stage_parse(const char *s); /* unknown/NULL -> OFF */
const char *wfe_enforce_stage_name(wfe_enforce_stage_t s);
/* 1 if the stage actually restricts (soft warns, hard refuses); advisory/off do not. */
int wfe_enforce_stage_restricts(wfe_enforce_stage_t s);
/* 1 if a denied action must be REFUSED (hard only); soft/advisory/off allow it. */
int wfe_enforce_stage_refuses(wfe_enforce_stage_t s);

/* The sliding-lease TTL in seconds (S2 step 6 watchdog): a bound work-item is
 * "stale" once this long has elapsed since its last MEANINGFUL advance. Read from
 * AIMEE_WORKFLOW_LEASE_TTL_SECS (a positive integer); defaults to 3600. A value of
 * 0 disables the lease (never stale). */
int wfe_lease_ttl_secs(void);

/* Compare-and-swap guard for an interactive advance_request (consult Q1). The
 * primary's advance carries the stage it OBSERVED; the engine advances only if
 * that still matches the work-item's ACTUAL current stage. Prevents a duplicate
 * chat turn / retry from double-advancing or advancing the wrong block. Returns:
 *   1  -> match, safe to advance;
 *   0  -> stale (observed != actual): the work-item already moved, reject;
 *  -1  -> bad args (NULL/empty). Caller layers nonce-based idempotency on top for
 *         the exact-replay case. */
int wfe_advance_cas_ok(const char *observed_stage, const char *actual_stage);

/* Build a user-facing enforcement message. TEMPLATED CONSTANT: only the gate name
 * and work-item id (both id-charset) are interpolated -- NEVER the primary's
 * attempted action text, file paths, or packet contents (injection/exfil vector,
 * consult Q3). `gate`/`work_item_id` are truncated/omitted if absent. */
void wfe_enforce_user_message(wfe_enforce_stage_t stage, const char *gate, const char *work_item_id,
                              char *buf, size_t n);

#endif /* DEC_WFE_ENFORCE_H */
