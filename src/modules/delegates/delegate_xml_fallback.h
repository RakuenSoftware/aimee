/* delegate_xml_fallback.h: XML tool-call parser for models without native tool calling */
#ifndef DEC_DELEGATE_XML_FALLBACK_H
#define DEC_DELEGATE_XML_FALLBACK_H 1

#include "agent_protocol.h"

/*
 * Parse XML-style tool calls from a model response text.
 *
 * Expected format:
 *   <tool_call>
 *     <name>tool_name</name>
 *     <arguments>{"key": "value"}</arguments>
 *   </tool_call>
 *
 * Fills out->calls[], out->call_count, and out->is_tool_call.
 * Any text before the first <tool_call> is stored in out->content.
 * Returns the number of tool calls found (0 if none).
 */
int xml_parse_tool_calls(const char *text, parsed_response_t *out);

/*
 * Same parser with control over bare/fenced JSON rescue. XML/Qwen/Channel
 * and Mistral bracket rescue stay enabled; allow_json controls only prose
 * JSON objects such as {"name":"bash","arguments":{...}}.
 */
int delegate_rescue_parse_tool_calls(const char *text, parsed_response_t *out, int allow_json);

/*
 * Backward-compatible detector with bare JSON rescue enabled.
 */
int delegate_rescue_has_tool_calls(const char *text);
int xml_has_tool_calls(const char *text);

/*
 * Same detector with control over prose JSON rescue.
 */
int delegate_rescue_has_tool_calls_with_json(const char *text, int allow_json);

#endif /* DEC_DELEGATE_XML_FALLBACK_H */
