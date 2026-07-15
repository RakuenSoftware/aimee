/* aimee_ir_rescue.h -- IR-side tool-call rescue for models without native tool calling.
 *
 * Some models emit tool calls as prose (XML <tool_call>, Qwen <function=>, harmony
 * channels, Mistral brackets, bare JSON) instead of using the provider's native
 * tool-call field. That is a MODEL CAPABILITY gap, not a legacy protocol, so the
 * dialect knowledge stays in delegate_xml_fallback.c and is reused verbatim here --
 * this module only moves it onto the IR's typed blocks.
 *
 * The structural payoff: the legacy path receives one flat string and must strip
 * reasoning textually (strip_reasoning_blocks) to avoid rescuing a tool call the
 * model was only THINKING about. The IR keeps reasoning in its own block type, so
 * this scans AIMEE_BLK_TEXT and never sees AIMEE_BLK_THINKING at all. The guarantee
 * is structural rather than heuristic. */
#ifndef DEC_AIMEE_IR_RESCUE_H
#define DEC_AIMEE_IR_RESCUE_H 1

#include "aimee_ir.h"

/* Rewrite prose tool calls in `r`'s TEXT blocks into real AIMEE_BLK_TOOL_USE blocks.
 *
 * No-ops (returns 0) when the response already carries a native TOOL_USE block:
 * native tool calling won, and rescuing on top of it would duplicate the call.
 *
 * `allow_json` gates prose-JSON rescue only (the agent_allows_json_content_rescue
 * policy); the XML/Qwen/channel/Mistral dialects are always eligible, matching
 * delegate_rescue_parse_tool_calls.
 *
 * Returns the number of tool calls rescued (0 if none). Increments the
 * ir_rescue_recoveries counter when it rescues. */
int aimee_ir_rescue_tool_calls(aimee_response_t *r, int allow_json);

#endif /* DEC_AIMEE_IR_RESCUE_H */
