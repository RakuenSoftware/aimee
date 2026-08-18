/* gw_stage_completion.h -- bounded completion-quality response intervention. */
#ifndef DEC_GW_STAGE_COMPLETION_H
#define DEC_GW_STAGE_COMPLETION_H 1

struct cJSON;
struct parsed_response;

/* True when the transcript is an active change session, a caller-owned shell
 * tool is available, and this transcript has a bounded completion attempt
 * remaining. Streaming ingresses use this to decide whether a
 * response must be buffered before it can pass through the response stage. */
int gw_response_completion_armed(const struct cJSON *messages, const struct cJSON *tools);

/* Return an allocated system prompt that makes Aimee's preferred registered
 * command surface the default discovery path when the caller supplies a shell
 * tool. Returns NULL when no compatible command surface is present. */
char *gw_request_tool_system_prompt(const struct cJSON *tools, const char *base_system_prompt);

/* If a text response explicitly identifies analogous production defects and
 * explicitly defers them, replace completion with one call to the caller's
 * existing shell tool. The command is deliberately inert: on the next ordinary
 * API turn Aimee recognizes the structured call id and applies its continuation
 * policy at the provider instruction layer, rather than treating tool output as
 * an instruction.
 * Returns 1 when it intervened, 0 otherwise. */
int gw_response_run_completion(struct parsed_response *parsed, const struct cJSON *messages,
                               const struct cJSON *tools, const char *tool_stop_reason);

/* Return an allocated system prompt with the bounded continuation policy
 * appended when messages contain Aimee's structured completion call. Returns
 * NULL when no continuation is pending. The caller owns the returned string. */
char *gw_request_completion_system_prompt(const struct cJSON *messages,
                                          const char *base_system_prompt);

/* Stable structured call-id prefix used to keep the intervention bounded. */
#define AIMEE_COMPLETION_CALL_PREFIX "call_aimee_completion_"

#endif /* DEC_GW_STAGE_COMPLETION_H */
