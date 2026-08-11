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
                                   parsed_response_t *out)
{
   (void)allow_json;
   if (!text)
      return 0;
   if (detect_only)
      return strstr(text, "<tool_call>") != NULL;
   if (!out)
      return 0;

   int found = 0;
   const char *p = text;
   const char *first = strstr(text, "<tool_call>");
   while (out->call_count < AGENT_MAX_TOOL_CALLS)
   {
      const char *block_end = NULL;
      const char *block = rescue_fixture_tag(p, "tool_call", &block_end);
      if (!block)
         break;

      const char *name_end = NULL, *args_end = NULL;
      const char *name = rescue_fixture_tag(block, "name", &name_end);
      const char *args = rescue_fixture_tag(block, "arguments", &args_end);
      if (name && name_end && name_end <= block_end)
      {
         parsed_tool_call_t *tc = &out->calls[out->call_count++];
         memset(tc, 0, sizeof(*tc));
         snprintf(tc->id, sizeof(tc->id), "xml_call_%d", out->call_count);
         size_t n = (size_t)(name_end - name);
         if (n >= sizeof(tc->name))
            n = sizeof(tc->name) - 1;
         memcpy(tc->name, name, n);
         if (args && args_end && args_end <= block_end)
         {
            size_t a = (size_t)(args_end - args);
            tc->arguments = malloc(a + 1);
            if (tc->arguments)
            {
               memcpy(tc->arguments, args, a);
               tc->arguments[a] = '\0';
            }
         }
         if (!tc->arguments)
            tc->arguments = strdup("{}");
         found++;
      }
      p = block_end + strlen("</tool_call>");
   }

   if (found > 0)
   {
      out->is_tool_call = 1;
      if (first && first > text)
      {
         size_t pre = (size_t)(first - text);
         while (pre > 0 && (text[pre - 1] == '\n' || text[pre - 1] == ' '))
            pre--;
         if (pre > 0 && !out->content)
         {
            out->content = malloc(pre + 1);
            if (out->content)
            {
               memcpy(out->content, text, pre);
               out->content[pre] = '\0';
            }
         }
      }
   }
   return found;
}

#endif /* DEC_TEST_RESCUE_FIXTURE_PROVIDER_H */
