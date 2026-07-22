/* aimee_ir_rescue.h -- IR-side tool-call rescue for models without native tool calling.
 *
 * Some models emit tool calls as prose (XML <tool_call>, Qwen <function=>, harmony
 * channels, Mistral brackets, bare JSON) instead of using the provider's native
 * tool-call field. That is a MODEL CAPABILITY gap, not a legacy protocol, so the
 * dialect knowledge stays behind delegate_rescue_parse_tool_calls and is reused
 * here; this module only maps the parser result onto the IR's typed blocks.
 *
 * The structural payoff: the legacy path receives one flat string and must strip
 * reasoning textually (strip_reasoning_blocks) to avoid rescuing a tool call the
 * model was only THINKING about. The IR keeps reasoning in its own block type, so
 * this scans AIMEE_BLK_TEXT and never sees AIMEE_BLK_THINKING at all. The guarantee
 * is structural rather than heuristic. */
#ifndef DEC_AIMEE_IR_RESCUE_H
#define DEC_AIMEE_IR_RESCUE_H 1

#include <aimee/ir/aimee_ir.h>

/* Rewrite prose tool calls in `r`'s AIMEE_BLK_TEXT blocks as
 * AIMEE_BLK_TOOL_USE blocks. AIMEE_BLK_THINKING and all other block types pass
 * through without parsing.
 *
 * If `r` already contains any AIMEE_BLK_TOOL_USE block, the function leaves the
 * entire response unchanged and returns 0.
 *
 * `allow_json` gates prose-JSON rescue only, according to
 * agent_allows_json_content_rescue. XML, Qwen, harmony-channel, and Mistral
 * dialects remain eligible regardless of this argument, matching
 * delegate_rescue_parse_tool_calls.
 *
 * On a successful rewrite, malformed or non-object arguments become an empty
 * JSON object, `r->stop_reason` becomes AIMEE_STOP_TOOL_USE, and
 * ir_rescue_recoveries is incremented once. The return value is the number of
 * calls rescued. A return value of 0 means no rewrite occurred: the input was null
 * or empty, the response already contained a tool-use block, no eligible call was
 * found, or a pre-commit rewrite allocation failed. In all cases `r` is left
 * unchanged.
 *
 * The caller retains ownership of `r`; successful rewriting replaces its content
 * array and transfers ownership of the resulting blocks to `r`. */
int aimee_ir_rescue_tool_calls(aimee_response_t *r, int allow_json);

#endif /* DEC_AIMEE_IR_RESCUE_H */
