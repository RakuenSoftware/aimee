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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* The standing exploration policy carried in every envelope. Kept short — it is
 * advice the model weighs, not a contract we can enforce over the wire. */
#define INGRESS_EXPLORE_WITH                                                                       \
   "find_symbol, lsp_references, ast_grep_search, search_graph, get_context_block"

#define INGRESS_AUDIT_CONTEXT_FILE            "audit_context.txt"
#define INGRESS_AUDIT_CONTEXT_MAX_AGE_SECONDS (6 * 60 * 60)

/* Per-request disable, set by the HTTP layer from the `x-aimee-preinject: 0`
 * header. Thread-local: the ingress runs the turn synchronously on the request
 * thread, so this is read by ingress_preinject_build() during the same request. */
static __thread int g_request_disabled = 0;

void ingress_preinject_set_request_disabled(int disabled)
{
   g_request_disabled = disabled ? 1 : 0;
}

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

char *ingress_preinject_format_code_block(const code_search_hit_t *hits, int n)
{
   if (!hits || n <= 0)
      return NULL;
   dstr_t d;
   dstr_init(&d);
   dstr_append_str(&d, "recommended (code):\n");
   for (int i = 0; i < n; i++)
   {
      dstr_appendf(&d, "  - %s\n", hits[i].file_path);
      /* One trimmed, single-line snippet so the agent sees why the file matched
       * without paying for the whole match. Collapse whitespace runs. */
      const char *s = hits[i].snippet;
      if (s && s[0])
      {
         char line[160];
         int j = 0;
         for (const char *p = s; *p && j < (int)sizeof(line) - 1; p++)
         {
            char c = (*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : *p;
            if (c == ' ' && (j == 0 || line[j - 1] == ' '))
               continue; /* skip leading / collapsed spaces */
            line[j++] = c;
         }
         while (j > 0 && line[j - 1] == ' ')
            j--;
         line[j] = '\0';
         if (line[0])
            dstr_appendf(&d, "    > %s\n", line);
      }
   }
   return dstr_steal(&d);
}

static char *ingress_preinject_read_audit_context(void)
{
   const char *dir = config_default_dir();
   if (!dir || !dir[0])
      return NULL;
   char path[4096];
   int n = snprintf(path, sizeof(path), "%s/%s", dir, INGRESS_AUDIT_CONTEXT_FILE);
   if (n < 0 || (size_t)n >= sizeof(path))
      return NULL;
   struct stat st;
   if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
      return NULL;
   time_t now = time(NULL);
   if (now == (time_t)-1 || st.st_mtime > now ||
       now - st.st_mtime > INGRESS_AUDIT_CONTEXT_MAX_AGE_SECONDS)
      return NULL;

   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   char *buf = malloc(2048);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t got = fread(buf, 1, 2047, f);
   fclose(f);
   buf[got] = '\0';
   if (got == 0)
   {
      free(buf);
      return NULL;
   }
   return buf;
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
   if (request_disabled || g_request_disabled)
      return NULL;
   if (!query || !query[0])
      return NULL;

   config_t cfg;
   config_load(&cfg);
   if (!cfg.ingress_preinject_enabled)
      return NULL;

   dstr_t block;
   dstr_init(&block);
   double score = 0.0;

   /* Primary signal: code search over the turn query. The code index is the
    * richest source, so recommended code files lead the envelope; the agent
    * sees which files matter before it explores. Confidence scales with how
    * many relevant files came back (no [0,1] rank is exposed by the search
    * path, so map the hit count into the tiering primitive). */
   code_search_hit_t hits[6];
   int n = kb_client_index_code_search(query, NULL, hits, (int)(sizeof(hits) / sizeof(hits[0])));
   if (n > 0)
   {
      char *code = ingress_preinject_format_code_block(hits, n);
      if (code)
      {
         dstr_append_str(&block, code);
         free(code);
      }
      double cs = (double)n / 6.0;
      if (cs > score)
         score = cs;
   }

   /* Secondary signal: durable memory context (the how/why), when present.
    * Empty in deployments with no charter, so guarded. */
   char *mem = kb_client_memory_context_block(query, "general", 5);
   if (mem && mem[0])
   {
      int lines = 0;
      for (const char *p = mem; *p; p++)
         if (*p == '\n')
            lines++;
      if (block.len)
         dstr_append_str(&block, "\n");
      dstr_append_str(&block, mem);
      double ms = lines >= 6 ? 0.7 : (lines >= 2 ? 0.4 : 0.1);
      if (ms > score)
         score = ms;
   }
   free(mem);

   char *audit = ingress_preinject_read_audit_context();
   if (audit && audit[0])
   {
      if (block.len)
         dstr_append_str(&block, "\n");
      dstr_append_str(&block, audit);
      if (score < 0.4)
         score = 0.4;
   }
   free(audit);

   char *blk = dstr_steal(&block);
   if (!blk || !blk[0])
   {
      free(blk);
      return NULL;
   }
   char *env = ingress_preinject_format_envelope(blk, ingress_preinject_confidence(score));
   free(blk);
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
