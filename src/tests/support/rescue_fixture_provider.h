/* rescue_fixture_provider.h -- the tool calls a test's own fixtures contain.
 *
 * Reading a tool call out of a model's prose is the delegates module's rule now
 * (server-go/modules/delegates/rescue*.go), pinned against the golden corpus at
 * server-go/modules/delegates/testdata/xml_fallback_golden.json. Test binaries
 * that reach it host no bus, and for every one of them the parse is SETUP: the
 * subject is what the caller does once a block has a call -- the IR refusing to
 * dispatch one found inside reasoning, the response pipeline rebuilding an
 * assistant message, a driver counting a rescued turn.
 *
 * So this reads back exactly the one spelling those fixtures are written in:
 *
 *   <tool_call><name>NAME</name><arguments>ARGS</arguments></tool_call>
 *
 * It is deliberately NOT the rule. It knows none of the other six dialects, has
 * no tool inventory to gate names against, strips no reasoning blocks and
 * normalizes nothing. It cannot drift into a second copy of a parser that lives
 * in exactly one place, because it never claims to be one -- if a fixture needs
 * behaviour beyond this shape, the case belongs in the Go tests with the rule.
 */
#ifndef DEC_TEST_RESCUE_FIXTURE_PROVIDER_H
#define DEC_TEST_RESCUE_FIXTURE_PROVIDER_H 1

#include "cJSON.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <aimee/delegates/delegate_xml_fallback.h>

static const char *rescue_fixture_tag(const char *from, const char *tag, const char **end_out)
{
   char open[32], close[32];
   snprintf(open, sizeof(open), "<%s>", tag);
   snprintf(close, sizeof(close), "</%s>", tag);
   const char *start = strstr(from, open);
   if (!start)
      return NULL;
   start += strlen(open);
   const char *end = strstr(start, close);
   if (!end)
      return NULL;
   *end_out = end;
   return start;
}

static int rescue_fixture_provider(const char *text, int allow_json, int detect_only,
                                   aimee_message_t *out)
{
   (void)allow_json;
   if (!text)
      return 0;
   if (detect_only)
      return strstr(text, "<tool_call>") != NULL;
   if (!out)
      return -1;
   const char *first = strstr(text, "<tool_call>"), *p = text;
   if (!first)
      return 0;
   out->role = strdup("assistant");
   if (!out->role)
      return -1;
   size_t pre = (size_t)(first - text);
   while (pre && (text[pre - 1] == ' ' || text[pre - 1] == '\n'))
      pre--;
   if (pre)
   {
      out->blocks = calloc(1, sizeof *out->blocks);
      if (!out->blocks)
         return -1;
      out->n_blocks = 1;
      out->blocks[0].type = AIMEE_BLK_TEXT;
      out->blocks[0].text = strndup(text, pre);
      if (!out->blocks[0].text)
         return -1;
   }
   int found = 0;
   while (1)
   {
      const char *end = NULL, *block = rescue_fixture_tag(p, "tool_call", &end);
      if (!block)
         break;
      const char *name_end = NULL, *args_end = NULL;
      const char *name = rescue_fixture_tag(block, "name", &name_end),
                 *args = rescue_fixture_tag(block, "arguments", &args_end);
      if (!name || !name_end || name_end > end || out->n_blocks >= AIMEE_IR_MAX_BLOCKS)
         return -1;
      aimee_block_t *blocks = realloc(out->blocks, (size_t)(out->n_blocks + 1) * sizeof *blocks);
      if (!blocks)
         return -1;
      out->blocks = blocks;
      aimee_block_t *call = &blocks[out->n_blocks++];
      memset(call, 0, sizeof *call);
      call->type = AIMEE_BLK_TOOL_USE;
      char id[64];
      snprintf(id, sizeof id, "xml_call_%d", ++found);
      call->tool_id = strdup(id);
      call->tool_name = strndup(name, (size_t)(name_end - name));
      char *arguments = args && args_end && args_end <= end
                            ? strndup(args, (size_t)(args_end - args))
                            : strdup("{}");
      call->tool_input = arguments ? cJSON_ParseWithOpts(arguments, NULL, 1) : NULL;
      free(arguments);
      if (!call->tool_id || !call->tool_name || !cJSON_IsObject(call->tool_input) ||
          !aimee_ir_json_unique(call->tool_input))
         return -1;
      p = end + strlen("</tool_call>");
   }
   return found;
}

#endif /* DEC_TEST_RESCUE_FIXTURE_PROVIDER_H */
