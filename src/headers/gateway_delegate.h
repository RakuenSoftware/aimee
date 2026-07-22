/* gateway_delegate.h: P4 of the universal gateway — route aimee's OWN outbound
 * model-call loop (primary + delegate, the shared agent_execute_with_tools_internal)
 * through the same gateway_pipeline the proxy ingresses use, so a config-enabled
 * tool-policing policy applies to delegate calls too.
 *
 * Scope is deliberately narrow: the loop runs ONLY the tool-policing stage. The
 * memory and model-pin stages are ingress concerns and are NOT run here — see
 * gateway_delegate.c for the rationale (memory would double-inject over the context
 * agent_build_exec_context_ex already assembles; model-pin would clobber the per-turn
 * fallback model). Both the request- and response-side policing are gated to delegate
 * calls (role != NULL); the primary shares the pipeline but is governed by the
 * agent-loop's own subagent guardrails, so policing it here would neuter aimee's own
 * delegation. */
#ifndef DEC_GATEWAY_DELEGATE_H
#define DEC_GATEWAY_DELEGATE_H 1

struct cJSON;

/* Where a tool entry's name lives in the freshly-built provider request body — the
 * flag gateway_policy_apply_request needs. Derived from the provider, not configured. */
typedef enum
{
   GW_TOOL_SHAPE_NAMED = 0,           /* anthropic + openai /responses: flat tool.name */
   GW_TOOL_SHAPE_FUNCTION_NESTED = 1, /* openai /chat/completions: tool.function.name */
   GW_TOOL_SHAPE_UNSUPPORTED = -1,    /* gemini: functionDeclarations — not policed (logged) */
} gw_tool_shape_t;

/* Map the provider flags already computed in the loop to a tool-name shape. A future
 * provider falls through to FUNCTION_NESTED only if it is neither anthropic, chatgpt
 * (responses), nor gemini — callers that add a provider with a novel tool shape must
 * extend this. */
gw_tool_shape_t gateway_delegate_tool_shape(int anthropic, int chatgpt);

/* Run the outbound request pipeline (tool-policing only) over a freshly-built provider
 * request `req`, mutating it in place. `is_delegate` (role != NULL) gates the strip:
 * for the primary it is a no-op (the call still goes "through the pipeline" — criterion
 * 5 — but does not police the primary's own tools). `shape` selects where tool names
 * live; GW_TOOL_SHAPE_UNSUPPORTED logs a warning when there were tools to police and
 * skips the strip (observable, not silent). `req` is BORROWED: the pipeline never frees
 * or replaces it (see gateway_pipeline.h). Returns total interventions (>=0) or <0 on a
 * hard stage error (the caller must then abort the turn without forwarding `req`). */
int gateway_delegate_run_request_pipeline(struct cJSON *req, gw_tool_shape_t shape,
                                          int is_delegate);

#endif /* DEC_GATEWAY_DELEGATE_H */
