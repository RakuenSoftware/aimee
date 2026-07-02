/* primary_cli_ingestor.h -- primary-as-manager S2 enforcement seam for the
 * Claude primary, which runs as a per-session tmux CLI TUI (cli_session): each
 * aimee session gets its OWN persistent interactive Claude pane, 1:1, so sessions
 * stay isolated and conversation history lives in the live pane. (`claude -p` is a
 * dead end here -- one-shot print mode cannot isolate or persist per-session
 * state, empirically confirmed.)
 *
 * Why this exists: the tmux Claude pane's model call reaches aimee out-of-band (a
 * separate HTTP connection/thread from the turn worker), so the in-process
 * session-id publish never reaches gw_stage_router and the S2 binding is INERT for
 * the Claude primary. This seam enforces at the IN-PROCESS turn seam instead: the
 * tmux turn is dispatched synchronously on the worker thread where the aimee
 * session id IS in scope, so binding right BEFORE the turn is sent to the pane
 * makes S1/S2 preventive for the turn. Additive + DEFAULT-OFF (see
 * primary_cli_ingestor_enabled + the AIMEE_WORKFLOW_ENFORCE_STAGE dial). */
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

#endif /* PRIMARY_CLI_INGESTOR_H */
