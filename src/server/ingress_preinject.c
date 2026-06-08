/* server/ingress_preinject.c: see ingress_preinject.h.
 *
 * The envelope is a compact, model-readable block. Its `explore-with` line
 * names Aimee's own retrieval tools so a co-registered agent fills any gap
 * THROUGH Aimee (symbol-scoped, graph-aware) instead of raw-grepping the tree.
 */
#include "ingress_preinject.h"
#include "config.h"
#include "kb_client.h"
#include "dstr.h"
#include <stdlib.h>
#include <string.h>

/* The standing exploration policy carried in every envelope. Kept short — it is
 * advice the model weighs, not a contract we can enforce over the wire. */
#define INGRESS_EXPLORE_WITH                                                                       \
   "find_symbol, lsp_references, ast_grep_search, search_graph, get_context_block"

const char *ingress_preinject_confidence(double top_score)
{
   /* Thresholds chosen so a clear top hit is "high", a plausible-but-thin match
    * is "medium", and a weak match is "low". Clamped to the documented tiers. */
   if (top_score >= 0.66)
      return "high";
   if (top_score >= 0.33)
      return "medium";
   return "low";
}

char *ingress_preinject_format_envelope(const char *context_block, const char *confidence)
{
   if (!context_block)
      return NULL;
   /* Treat a whitespace-only block as empty → no envelope. */
   const char *p = context_block;
   while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
      p++;
   if (*p == '\0')
      return NULL;

   if (!confidence || !confidence[0])
      confidence = "low";

   dstr_t d;
   dstr_init(&d);
   dstr_appendf(&d, "<aimee-context confidence=\"%s\">\n", confidence);
   dstr_append_str(&d, context_block);
   if (context_block[strlen(context_block) - 1] != '\n')
      dstr_append_str(&d, "\n");
   dstr_append_str(&d, "explore-with: " INGRESS_EXPLORE_WITH "\n");
   dstr_append_str(&d, "</aimee-context>");
   char *out = dstr_steal(&d);
   return out;
}

/* Pull the text out of a message `content` that is either a JSON string or an
 * array of {type, text} parts (the Responses content shape). Appends into d. */
static void append_content_text(dstr_t *d, const cJSON *content)
{
   if (cJSON_IsString(content))
   {
      dstr_append_str(d, content->valuestring);
      return;
   }
   if (cJSON_IsArray(content))
   {
      const cJSON *part = NULL;
      cJSON_ArrayForEach(part, content)
      {
         const cJSON *t = cJSON_GetObjectItemCaseSensitive(part, "text");
         if (cJSON_IsString(t))
            dstr_append_str(d, t->valuestring);
      }
   }
}

char *ingress_preinject_query_from_messages(const cJSON *messages)
{
   if (!cJSON_IsArray(messages))
      return NULL;

   /* Walk to the LAST user-role message — that is the current turn's ask. */
   const cJSON *msg = NULL;
   const cJSON *last_user = NULL;
   cJSON_ArrayForEach(msg, messages)
   {
      const cJSON *role = cJSON_GetObjectItemCaseSensitive(msg, "role");
      if (cJSON_IsString(role) && strcmp(role->valuestring, "user") == 0)
         last_user = msg;
   }
   if (!last_user)
      return NULL;

   dstr_t d;
   dstr_init(&d);
   append_content_text(&d, cJSON_GetObjectItemCaseSensitive(last_user, "content"));
   char *out = dstr_steal(&d);
   if (out && out[0] == '\0')
   {
      free(out);
      return NULL;
   }
   return out;
}

char *ingress_preinject_build(const char *query, int request_disabled)
{
   if (request_disabled)
      return NULL;
   if (!query || !query[0])
      return NULL;

   config_t cfg;
   config_load(&cfg);
   if (!cfg.ingress_preinject_enabled)
      return NULL;

   char *block = kb_client_memory_context_block(query, "general", 5);
   if (!block)
      return NULL;

   /* No score is exposed by the context-block path yet, so derive a coarse
    * confidence from how much context came back: a multi-entry block is a
    * stronger signal than a sparse one. This is the provisional source for the
    * (unit-tested) ingress_preinject_confidence() tiering until the recall path
    * surfaces graph_score. */
   int lines = 0;
   for (const char *p = block; *p; p++)
      if (*p == '\n')
         lines++;
   double score = lines >= 6 ? 0.7 : (lines >= 2 ? 0.4 : 0.1);

   char *env = ingress_preinject_format_envelope(block, ingress_preinject_confidence(score));
   free(block);
   return env;
}

char *ingress_preinject_apply(const char *instructions, const char *envelope)
{
   int env_blank = 1;
   if (envelope)
      for (const char *p = envelope; *p; p++)
         if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
         {
            env_blank = 0;
            break;
         }

   if (env_blank)
      return instructions ? strdup(instructions) : NULL;

   dstr_t d;
   dstr_init(&d);
   dstr_append_str(&d, envelope);
   dstr_append_str(&d, "\n\n");
   if (instructions && instructions[0])
      dstr_append_str(&d, instructions);
   return dstr_steal(&d);
}
