/* gen_xml_fallback_golden.c: emit the C parser's output for the shared corpus.
 *
 * This is the reference half of a differential port. The Go implementation in
 * server-go/modules/delegates is asserted against this file, so a divergence in
 * any shape is a test failure naming that shape rather than a surprise in
 * production.
 *
 * Writes JSON to stdout; the build target refreshes
 * server-go/modules/delegates/testdata/xml_fallback_golden.json.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee.h"
#include "agent_protocol.h"
#include "cJSON.h"
#include <aimee/delegates/delegate_xml_fallback.h>

#include "support/xml_fallback_corpus.h"

/* The rescue gates unknown tool names on the registry. Keep the declared set
 * small and explicit so the Go side can be handed the same list. */
static const char *KNOWN_TOOLS[] = {"bash", "read"};

struct cJSON *agent_tool_get_schema_cached(const char *tool_name)
{
   if (!tool_name)
      return NULL;
   for (size_t i = 0; i < sizeof(KNOWN_TOOLS) / sizeof(KNOWN_TOOLS[0]); i++)
      if (strcmp(tool_name, KNOWN_TOOLS[i]) == 0)
         return cJSON_CreateObject();
   return NULL;
}

void agent_free_parsed_response(parsed_response_t *p)
{
   if (!p)
      return;
   for (int i = 0; i < p->call_count; i++)
      free(p->calls[i].arguments);
   free(p->content);
   if (p->assistant_message)
      cJSON_Delete(p->assistant_message);
   memset(p, 0, sizeof(*p));
}

int main(void)
{
   cJSON *root = cJSON_CreateObject();
   cJSON *tools = cJSON_AddArrayToObject(root, "known_tools");
   for (size_t i = 0; i < sizeof(KNOWN_TOOLS) / sizeof(KNOWN_TOOLS[0]); i++)
      cJSON_AddItemToArray(tools, cJSON_CreateString(KNOWN_TOOLS[i]));

   cJSON *cases = cJSON_AddArrayToObject(root, "cases");
   for (int i = 0; i < XML_CORPUS_COUNT; i++)
   {
      const xml_corpus_entry_t *e = &XML_CORPUS[i];
      parsed_response_t r;
      memset(&r, 0, sizeof(r));
      int rc = delegate_rescue_parse_tool_calls(e->text, &r, e->allow_json);

      cJSON *c = cJSON_CreateObject();
      cJSON_AddStringToObject(c, "name", e->name);
      cJSON_AddStringToObject(c, "text", e->text);
      cJSON_AddBoolToObject(c, "allow_json", e->allow_json ? 1 : 0);
      cJSON_AddNumberToObject(c, "rc", rc);
      cJSON_AddBoolToObject(c, "is_tool_call", r.is_tool_call ? 1 : 0);
      /* NULL and "" are different outcomes here, so record which it was. */
      if (r.content)
         cJSON_AddStringToObject(c, "content", r.content);
      else
         cJSON_AddNullToObject(c, "content");
      cJSON_AddBoolToObject(c, "detected", delegate_rescue_has_tool_calls(e->text) ? 1 : 0);
      cJSON_AddBoolToObject(
          c, "detected_json",
          delegate_rescue_has_tool_calls_with_json(e->text, e->allow_json) ? 1 : 0);

      cJSON *calls = cJSON_AddArrayToObject(c, "calls");
      for (int k = 0; k < r.call_count; k++)
      {
         cJSON *call = cJSON_CreateObject();
         cJSON_AddStringToObject(call, "id", r.calls[k].id);
         cJSON_AddStringToObject(call, "name", r.calls[k].name);
         if (r.calls[k].arguments)
            cJSON_AddStringToObject(call, "arguments", r.calls[k].arguments);
         else
            cJSON_AddNullToObject(call, "arguments");
         cJSON_AddItemToArray(calls, call);
      }
      cJSON_AddItemToArray(cases, c);
      agent_free_parsed_response(&r);
   }

   char *out = cJSON_Print(root);
   if (!out)
   {
      cJSON_Delete(root);
      return 1;
   }
   printf("%s\n", out);
   free(out);
   cJSON_Delete(root);
   return 0;
}
