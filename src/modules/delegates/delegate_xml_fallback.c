/* delegate_xml_fallback.c: entry points for recovering tool calls a model wrote
 * as text instead of making them properly.
 *
 * The dialects -- <tool_call> blocks and their namespaced form, Qwen's
 * <function=>, <invoke>, harmony's <|channel>call:, Mistral's [TOOL_CALLS] and
 * bare JSON -- were ~1100 lines of scanning here. Reading a model's prose and
 * deciding it contains a call is a decision, so it is now
 * server-go/modules/delegates/rescue*.go, pinned against the same golden corpus
 * this file was pinned against (tests/gen_xml_fallback_golden.c).
 *
 * Fails closed as NOTHING RESCUED. With no answer the response is treated as
 * ordinary prose, which is what it looked like in the first place. The other
 * direction -- inventing a call -- would run a tool the model never asked for.
 */
#include "aimee.h"
#include <aimee/delegates/delegate_xml_fallback.h>
#include <string.h>

static delegate_rescue_provider_fn g_rescue_provider;

void delegate_register_rescue_provider(delegate_rescue_provider_fn provider)
{
   g_rescue_provider = provider;
}

int delegate_rescue_parse_tool_calls(const char *text, parsed_response_t *out, int allow_json)
{
   if (!text || !out || !g_rescue_provider)
      return 0;
   return g_rescue_provider(text, allow_json, 0, out);
}

int xml_parse_tool_calls(const char *text, parsed_response_t *out)
{
   return delegate_rescue_parse_tool_calls(text, out, 1);
}

int delegate_rescue_has_tool_calls_with_json(const char *text, int allow_json)
{
   if (!text || !g_rescue_provider)
      return 0;
   return g_rescue_provider(text, allow_json, 1, NULL) > 0;
}

int delegate_rescue_has_tool_calls(const char *text)
{
   return delegate_rescue_has_tool_calls_with_json(text, 1);
}

int xml_has_tool_calls(const char *text)
{
   return delegate_rescue_has_tool_calls(text);
}
