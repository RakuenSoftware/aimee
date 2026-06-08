/* cli_attention_guard.c: see cli_attention_guard.h.
 *
 * Per-session attention log lives at $AIMEE_HOME/.cache/attention/<session>.json
 * as an array of {path, weight, ts}. On each PreToolUse the guard accrues the
 * current tool's attention (Read=2, edit-class=8) and, for a hard-destructive
 * Bash command, blocks (exit 2) when the command text mentions a path the log
 * scores at/above the high-attention threshold. Substring-matching known
 * high-attention paths against the command avoids fragile shell parsing.
 */
#include "cli_attention_guard.h"
#include "cli_session_start.h" /* read_stdin */
#include "aimee_home.h"
#include "platform_path.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

double attn_score(const attn_record_t *recs, int n, const char *path, long now_ts)
{
   if (!recs || !path)
      return 0.0;
   double total = 0.0;
   for (int i = 0; i < n; i++)
   {
      if (!recs[i].path || strcmp(recs[i].path, path) != 0)
         continue;
      double age_hours = (double)(now_ts - recs[i].ts) / 3600.0;
      if (age_hours < 0.0)
         age_hours = 0.0;
      total += (double)recs[i].weight * pow(0.5, age_hours);
   }
   return total;
}

int attn_weight_for(attn_op_t op)
{
   return op == ATTN_OP_READ ? 2 : 8;
}

/* True if `cmd` contains a hard-destructive pattern. Conservative. */
static int bash_is_hard(const char *cmd)
{
   if (!cmd || !cmd[0])
      return 0;
   /* rm with both -r and -f (in any order / combined form). */
   const char *rm = strstr(cmd, "rm ");
   if (rm)
   {
      int has_r = (strstr(rm, "-r") || strstr(rm, "-fr") || strstr(rm, "-rf") || strstr(rm, "-R"));
      int has_f = (strstr(rm, "-f") || strstr(rm, "-fr") || strstr(rm, "-rf"));
      if (has_r && has_f)
         return 1;
   }
   if (strstr(cmd, "truncate ") || strstr(cmd, "shred ") || strstr(cmd, "mkfs") ||
       strstr(cmd, "dd if=/dev/zero") || strstr(cmd, ": >") || strstr(cmd, ":>"))
      return 1;
   return 0;
}

attn_op_t attn_classify(const char *tool_name, const char *bash_cmd)
{
   if (!tool_name)
      return ATTN_OP_READ;
   if (strcmp(tool_name, "Read") == 0)
      return ATTN_OP_READ;
   if (strcmp(tool_name, "Edit") == 0 || strcmp(tool_name, "Write") == 0 ||
       strcmp(tool_name, "MultiEdit") == 0 || strcmp(tool_name, "NotebookEdit") == 0)
      return ATTN_OP_SOFT;
   if (strcmp(tool_name, "Bash") == 0)
   {
      if (bash_is_hard(bash_cmd))
         return ATTN_OP_HARD;
      if (bash_cmd && (strstr(bash_cmd, "rm ") || strstr(bash_cmd, " > ")))
         return ATTN_OP_SOFT;
      return ATTN_OP_READ;
   }
   return ATTN_OP_READ;
}

/* ---- JSON log persistence (impure) ---- */

#define ATTN_MAX_RECORDS    1024
#define ATTN_PRUNE_AGE_SECS (24 * 3600)

static void attn_log_path(const char *session_id, char *out, size_t cap)
{
   const char *home = aimee_home();
   if (!home || !home[0])
      home = "/tmp";
   /* Sanitize the session id into a filename. */
   char sid[128];
   int j = 0;
   const char *s = (session_id && session_id[0]) ? session_id : "default";
   for (; *s && j < (int)sizeof(sid) - 1; s++)
   {
      char c = *s;
      sid[j++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_')
                     ? c
                     : '_';
   }
   sid[j] = '\0';
   snprintf(out, cap, "%s/.cache/attention/%s.json", home, sid);
}

static cJSON *attn_load(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return cJSON_CreateArray();
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   cJSON *arr = NULL;
   if (sz > 0 && sz < (1 << 20))
   {
      char *buf = malloc((size_t)sz + 1);
      if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz)
      {
         buf[sz] = '\0';
         arr = cJSON_Parse(buf);
      }
      free(buf);
   }
   fclose(f);
   if (!cJSON_IsArray(arr))
   {
      cJSON_Delete(arr);
      arr = cJSON_CreateArray();
   }
   return arr;
}

static void attn_save(const char *path, cJSON *arr, long now_ts)
{
   /* Prune old/excess records: drop entries older than 24h, cap total. */
   int n = cJSON_GetArraySize(arr);
   for (int i = n - 1; i >= 0; i--)
   {
      cJSON *e = cJSON_GetArrayItem(arr, i);
      cJSON *ts = cJSON_GetObjectItemCaseSensitive(e, "ts");
      if (cJSON_IsNumber(ts) && (now_ts - (long)ts->valuedouble) > ATTN_PRUNE_AGE_SECS)
         cJSON_DeleteItemFromArray(arr, i);
   }
   while (cJSON_GetArraySize(arr) > ATTN_MAX_RECORDS)
      cJSON_DeleteItemFromArray(arr, 0);

   char dir[1024];
   snprintf(dir, sizeof(dir), "%s", path);
   char *slash = strrchr(dir, '/');
   if (slash)
   {
      *slash = '\0';
      platform_mkdir_p(dir, 0700);
   }
   char *json = cJSON_PrintUnformatted(arr);
   if (json)
   {
      FILE *f = fopen(path, "wb");
      if (f)
      {
         fputs(json, f);
         fclose(f);
      }
      free(json);
   }
}

/* Build a flat attn_record_t view over the JSON array (path pointers borrow the
 * cJSON strings, valid while `arr` lives). Returns count. */
static int attn_records_from_json(cJSON *arr, attn_record_t *out, int max)
{
   int n = 0;
   cJSON *e = NULL;
   cJSON_ArrayForEach(e, arr)
   {
      if (n >= max)
         break;
      cJSON *p = cJSON_GetObjectItemCaseSensitive(e, "path");
      cJSON *w = cJSON_GetObjectItemCaseSensitive(e, "weight");
      cJSON *ts = cJSON_GetObjectItemCaseSensitive(e, "ts");
      if (!cJSON_IsString(p) || !cJSON_IsNumber(w) || !cJSON_IsNumber(ts))
         continue;
      out[n].path = p->valuestring;
      out[n].weight = (int)w->valuedouble;
      out[n].ts = (long)ts->valuedouble;
      n++;
   }
   return n;
}

static void attn_record(cJSON *arr, const char *path, int weight, long now_ts)
{
   if (!path || !path[0])
      return;
   cJSON *e = cJSON_CreateObject();
   cJSON_AddStringToObject(e, "path", path);
   cJSON_AddNumberToObject(e, "weight", weight);
   cJSON_AddNumberToObject(e, "ts", (double)now_ts);
   cJSON_AddItemToArray(arr, e);
}

int handle_attention_guard(void)
{
   char *stdin_data = read_stdin();
   cJSON *hook = stdin_data ? cJSON_Parse(stdin_data) : NULL;
   if (!hook)
   {
      free(stdin_data);
      return 0; /* malformed input — never block */
   }

   const char *tool = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(hook, "tool_name"));
   const char *sid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(hook, "session_id"));
   cJSON *ti = cJSON_GetObjectItemCaseSensitive(hook, "tool_input");
   const char *bash_cmd =
       ti ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(ti, "command")) : NULL;

   long now_ts = (long)time(NULL);
   attn_op_t op = attn_classify(tool, bash_cmd);

   char path[1024];
   attn_log_path(sid, path, sizeof(path));
   cJSON *arr = attn_load(path);

   int exit_code = 0;

   if (op == ATTN_OP_HARD && bash_cmd && bash_cmd[0])
   {
      /* Block if the destructive command mentions a high-attention path. */
      attn_record_t recs[ATTN_MAX_RECORDS];
      int n = attn_records_from_json(arr, recs, ATTN_MAX_RECORDS);
      /* Dedup the paths we score so we don't repeat work. */
      for (int i = 0; i < n; i++)
      {
         const char *p = recs[i].path;
         if (!p || !p[0] || !strstr(bash_cmd, p))
            continue;
         if (attn_score(recs, n, p, now_ts) >= ATTN_HIGH_THRESHOLD)
         {
            fprintf(stderr,
                    "aimee attention-guard: blocked a destructive command targeting '%s', a "
                    "file this session has actively read/edited. Re-run with intent if this is "
                    "deliberate (the guard only blocks hard-destructive ops on high-attention "
                    "files).\n",
                    p);
            exit_code = 2;
            break;
         }
      }
   }
   else if (ti)
   {
      /* Accrue attention for the touched file (Read / edit-class). */
      const char *keys[] = {"file_path", "path", "notebook_path"};
      for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
      {
         const char *fp = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(ti, keys[k]));
         if (fp && fp[0])
            attn_record(arr, fp, attn_weight_for(op), now_ts);
      }
   }

   attn_save(path, arr, now_ts);
   cJSON_Delete(arr);
   cJSON_Delete(hook);
   free(stdin_data);
   return exit_code;
}
