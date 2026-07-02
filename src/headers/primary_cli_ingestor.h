/* primary_cli_ingestor.h -- primary-as-manager S2 enforcement seam for the
 * external-CLI primary (Claude Code driven over `-p --output-format stream-json`).
 *
 * Why this exists: the Claude primary MUST run as an external CLI. Its model
 * calls reach aimee out-of-band (a separate HTTP connection/thread from the turn
 * worker), so the in-process session-id publish never reaches gw_stage_router and
 * the S2 binding is INERT for the Claude primary. This adapter enforces at the
 * IN-PROCESS turn seam instead: BEFORE the turn is sent to the CLI it routes +
 * binds the session (S1/S2), so the primary is under management for the turn.
 *
 * Slice 2 scope: the gate + the enforce-before-send seam only. Driving the
 * agent_shell stream-json backend + rendering its events is a later slice. The
 * whole path is additive and DEFAULT-OFF (see primary_cli_ingestor_enabled). */
#ifndef PRIMARY_CLI_INGESTOR_H
#define PRIMARY_CLI_INGESTOR_H

/* Opt-in gate: AIMEE_PRIMARY_CLI_INGESTOR in {1,on,true}. Default OFF -- the
 * primary keeps its existing execution path untouched unless explicitly enabled. */
int primary_cli_ingestor_enabled(void);

/* Enforce the S1 route + S2 bind/guard for THIS turn, BEFORE it is sent to the
 * CLI (preventive for the turn, not detective on its output).
 *
 *   session_id: the aimee session id in scope on the turn worker thread
 *   message:    the user turn text (routed to decide enforced vs generic)
 *   repo:       optional repo root for the work-item (may be NULL)
 *
 * Trust boundary: MUST be called with a resolved session id. An empty/NULL id is
 * a no-op returning 0 -- never a silent "pretend-enforced" that binds nothing.
 * Returns 1 if the session is now bound (enforcement active this turn), else 0. */
int primary_cli_ingestor_enforce_preturn(const char *session_id, const char *message,
                                         const char *repo);

/* Result of one ingested primary turn. All heap fields are caller-owned; free via
 * primary_cli_turn_result_free. */
typedef struct
{
   char *text;      /* accumulated assistant text (owned; NULL if none) */
   char *session;   /* backend session id to resume the NEXT turn (owned; NULL if none) */
   char error[256]; /* first error seen (empty if none) */
   int bound;       /* 1 if this turn is under S2 management (enforced+routed) */
   int tool_calls;  /* count of native CLI tool_start events observed (audit) */
} primary_cli_turn_result_t;

void primary_cli_turn_result_free(primary_cli_turn_result_t *r);

/* Drive ONE primary turn through the agent_shell CLI backend (Slice 3), ingesting
 * its stream events into `out`. Enforcement (S1 route + S2 bind/guard) runs BEFORE
 * the turn is sent to the CLI, so it is preventive for the turn (out->bound reports
 * whether the session is now managed). Native CLI tool events are observed for
 * audit only (detective, not preventive -- the shell-tool bypass is a separate
 * track).
 *   session_id: aimee session id in scope on the turn worker thread (empty => the
 *               enforce step is a no-op, but the turn still runs unmanaged)
 *   driver_name: agent_shell driver ("claude" when NULL/empty)
 *   resume_id:  backend session id to resume (NULL for a fresh backend session)
 *   interrupted: optional cooperative-cancel flag (may be NULL)
 * Returns 0 on clean completion, -1 on error (out->error carries the message).
 * `out` must be non-NULL; it is fully overwritten and owned by the caller. */
int primary_cli_ingestor_turn(const char *session_id, const char *message, const char *repo,
                              const char *driver_name, const char *resume_id,
                              primary_cli_turn_result_t *out, volatile int *interrupted);

#endif /* PRIMARY_CLI_INGESTOR_H */
