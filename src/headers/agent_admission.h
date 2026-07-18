#ifndef AGENT_ADMISSION_H
#define AGENT_ADMISSION_H

/* agent_admission: the SINGLE admission controller every aimee-run agent turn passes
 * through, whatever the producer (chat front-end, delegate, WFE panel, ensemble, aux,
 * cron/trigger, fallback peer). It is the one place concurrency is bounded, and it is
 * FAIL-CLOSED: any missing/invalid/unconfigured input is rejected, never waved through.
 *
 * A slot represents one live agent EXECUTION CONTEXT, identified by its handle:
 *   - delegate            -> its delegation_id
 *   - persistent CLI pane -> the pane/session id (e.g. "<sid>-cli", "<sid>-<deleg>")
 *   - one-shot HTTP/panel/aux turn -> a fresh per-invocation id
 * Every turn WITHIN one context reuses that context's single slot (multi-turn panes,
 * tool loops, nested sub-turns) — so nesting never re-contends and cannot self-deadlock.
 * Two concurrent uses of the same agent are two contexts and therefore two slots.
 *
 * Each acquire enforces THREE limits together, atomically under one lock:
 *   1. global   — maximum_total_concurrent_agent_sessions (all agents, all contexts)
 *   2. per-agent — the agent's max_parallel
 *   3. per-model — the model's configured concurrency cap (agents can share a model)
 *
 * The only turns exempt from admission are (a) the externally-driven MCP/ACP top-level
 * session (aimee does not own its concurrency; everything it spawns IS admitted) and
 * (b) low-level health/connectivity probes. Those never call acquire.
 */

/* Default global ceiling when maximum_total_concurrent_agent_sessions is unset (0). */
#define AGENT_ADMISSION_DEFAULT_GLOBAL_MAX 14

typedef struct agent_slot agent_slot_t;

typedef enum
{
   AGENT_ADMIT_OK = 0,   /* admitted; caller holds a slot and MUST release it */
   AGENT_ADMIT_AT_LIMIT, /* at capacity: non-blocking try failed, or blocked wait was cancelled */
   AGENT_ADMIT_INVALID,  /* FAIL-CLOSED: null/empty/<=0/unconfigured/uninitialized input */
   AGENT_ADMIT_CANCELLED /* the caller's cancel callback fired while queued */
} agent_admit_status_t;

/* Polled (~every 100ms) while a turn is blocked waiting for a slot; return nonzero to
 * abandon the wait (e.g. the delegation was stopped). NULL means "never cancel". */
typedef int (*agent_admit_cancel_fn)(const char *cancel_ctx);

#define AGENT_ADMIT_NONBLOCKING 0x1u /* try-acquire: reject with AT_LIMIT instead of blocking */

/* Higher priority is admitted first when capacity frees. */
#define AGENT_ADMIT_PRIORITY_INTERACTIVE 0
#define AGENT_ADMIT_PRIORITY_BACKGROUND (-10)

typedef struct
{
   const char *ctx_handle;   /* REQUIRED: execution-context id (see file header) */
   const char *agent;        /* REQUIRED: agent name — the per-agent limit key */
   const char *model;        /* REQUIRED: model name — the per-model limit key */
   int per_agent_max;        /* REQUIRED > 0: this agent's max_parallel */
   int priority;             /* AGENT_ADMIT_PRIORITY_* (default 0) */
   unsigned flags;           /* AGENT_ADMIT_NONBLOCKING */
   agent_admit_cancel_fn cancel_fn; /* optional */
   const char *cancel_ctx;          /* passed to cancel_fn */
} agent_admit_req_t;

typedef struct
{
   char model[64];
   int limit; /* > 0 */
} agent_admission_model_limit_t;

/* (Re)configure the caps. Idempotent and safe to call repeatedly (config hot-reload).
 * A global_max <= 0 leaves the controller UNCONFIGURED, so every acquire fails closed
 * until a valid cap is set. default_model_limit applies to any model not in overrides. */
void agent_admission_configure(int global_max, int default_model_limit,
                               const agent_admission_model_limit_t *overrides, int override_count);

/* Acquire a slot for req. On success returns a non-NULL slot and sets *status = OK.
 * On any failure returns NULL and sets *status to the reason. `status` may be NULL. */
agent_slot_t *agent_admission_acquire(const agent_admit_req_t *req, agent_admit_status_t *status);

/* Release a slot obtained from acquire. Safe with NULL. Decrements the context's
 * refcount; the global/per-agent/per-model counts drop only when the context's last
 * holder releases. */
void agent_admission_release(agent_slot_t *slot);

/* Test/inspection helpers (also used by assertions). Return -1 if unconfigured. */
int agent_admission_global_active(void);
int agent_admission_agent_active(const char *agent);

#endif /* AGENT_ADMISSION_H */
