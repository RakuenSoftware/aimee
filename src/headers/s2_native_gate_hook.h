/* s2_native_gate_hook.h -- server-side S2 pre-delivery native-tool gate for the
 * hooks.pre RPC (tracks 2+3). This is the REAL enforcement point: `aimee hooks pre`
 * forwards to hooks.pre, so the gate must run here (the CLI cmd_hooks copy is only a
 * server-unreachable fallback). Honest scope + fail policy: see wfe_native_gate.h. */
#ifndef DEC_S2_NATIVE_GATE_HOOK_H
#define DEC_S2_NATIVE_GATE_HOOK_H 1

#include "server.h" /* server_conn_t */

/* If this tool call must be DENIED (an externalizing native tool used by a session
 * bound to an enforced work-item that has not passed gate.deliver), send the blocked
 * PreToolUse response on `conn` and return the send rc (>=0). Return -1 if allowed
 * (the caller continues to the generic guardrails). */
int s2_native_gate_hook_pre(server_conn_t *conn, const char *sid, const char *tool_name,
                            const char *tool_input, const char *request_id);


/* Send a "blocked" PreToolUse response (status=blocked, exit_code 2, message) on
 * `conn` and return the send rc. Shared by the S2 gate and other hooks.pre denies. */
int hook_send_blocked(server_conn_t *conn, const char *msg, const char *request_id);

#endif /* DEC_S2_NATIVE_GATE_HOOK_H */
