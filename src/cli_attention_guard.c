/* cli_attention_guard.c: see cli_attention_guard.h.
 *
 * Per-session attention log lives at $AIMEE_HOME/.cache/attention/<session>.json
 * as an array of {path, weight, ts}. On each PreToolUse the guard accrues the
 * current tool's attention (Read=2, edit-class=8) and, for a hard-destructive
 * Bash command, blocks (exit 2) when the command text mentions a path the log
 * scores at/above the high-attention threshold. Substring-matching known
 * high-attention paths against the command avoids fragile shell parsing.
 * Recursive raw scans are blocked by default and redirected toward Aimee's
 * indexed exploration tools; ingress_max_raw_scans allows a capped number per
 * session.
 */
#include "cli_attention_guard.h"
#include "cli_session_start.h" /* read_stdin */
#include "aimee_home.h"
#include "platform_path.h"
#include "cJSON.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
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

static int cmd_has_token(const char *cmd, const char *tok)
{
   if (!cmd || !tok || !tok[0])
      return 0;
   size_t n = strlen(tok);
   const char *p = cmd;
   while ((p = strstr(p, tok)) != NULL)
   {
      int left = (p == cmd || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n');
      char r = p[n];
      int right = (r == '\0' || r == ' ' || r == '\t' || r == '\n');
      if (left && right)
         return 1;
      p += n;
   }
   return 0;
}

static int bash_is_raw_recursive_scan(const char *cmd)
{
   if (!cmd || !cmd[0])
      return 0;
   int names_tool = (cmd_has_token(cmd, "grep") || cmd_has_token(cmd, "rg") ||
                     cmd_has_token(cmd, "find") || cmd_has_token(cmd, "ls"));
   if (!names_tool)
      return 0;
   return strstr(cmd, "grep -r") || strstr(cmd, "grep -R") || strstr(cmd, "rg --files") ||
          strstr(cmd, "find .") || strstr(cmd, "ls -R") || strstr(cmd, "cat $(find") ||
          strstr(cmd, "xargs grep");
}

int attn_is_raw_scan(const char *tool_name, const char *bash_cmd)
{
   if (!tool_name)
      return 0;
   if (strcmp(tool_name, "Grep") == 0 || strcmp(tool_name, "Glob") == 0)
      return 1;
   if (strcmp(tool_name, "Bash") == 0)
      return bash_is_raw_recursive_scan(bash_cmd);
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
      if (attn_is_raw_scan(tool_name, bash_cmd))
         return ATTN_OP_RAW_SCAN;
      if (bash_cmd && (strstr(bash_cmd, "rm ") || strstr(bash_cmd, " > ")))
         return ATTN_OP_SOFT;
      return ATTN_OP_READ;
   }
   if (attn_is_raw_scan(tool_name, bash_cmd))
      return ATTN_OP_RAW_SCAN;
   return ATTN_OP_READ;
}

/* ---- JSON log persistence (impure) ---- */

#define ATTN_MAX_RECORDS    1024
#define ATTN_PRUNE_AGE_SECS (24 * 3600)
#define ATTN_RAW_SCAN_PATH  "__aimee_raw_scan__"

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

static int attn_raw_scan_count(cJSON *arr, long now_ts)
{
   int count = 0;
   cJSON *e = NULL;
   cJSON_ArrayForEach(e, arr)
   {
      cJSON *p = cJSON_GetObjectItemCaseSensitive(e, "path");
      cJSON *ts = cJSON_GetObjectItemCaseSensitive(e, "ts");
      if (!cJSON_IsString(p) || strcmp(p->valuestring, ATTN_RAW_SCAN_PATH) != 0 ||
          !cJSON_IsNumber(ts))
         continue;
      long age = now_ts - (long)ts->valuedouble;
      if (age >= 0 && age <= ATTN_PRUNE_AGE_SECS)
         count++;
   }
   return count;
}

static int attn_parse_nonnegative_int(const char *s, int *out)
{
   if (!s || !out)
      return 0;

   while (isspace((unsigned char)*s))
      s++;
   if (*s == '+')
      s++;
   if (!isdigit((unsigned char)*s))
      return 0;

   errno = 0;
   char *end = NULL;
   long value = strtol(s, &end, 10);
   if (errno || end == s || value < 0 || value > INT_MAX)
      return 0;
   *out = (int)value;
   return 1;
}

static int attn_parse_ingress_max_raw_scans(const char *buf, int *out)
{
   if (!buf || !out)
      return 0;

   const char *line = buf;
   while (*line)
   {
      const char *p = line;
      while (*p == ' ' || *p == '\t')
         p++;

      if (*p && *p != '#')
      {
         char quote = 0;
         if (*p == '"' || *p == '\'')
            quote = *p++;

         size_t key_len = strlen("ingress_max_raw_scans");
         if (strncmp(p, "ingress_max_raw_scans", key_len) == 0)
         {
            p += key_len;
            if (quote)
            {
               if (*p != quote)
                  goto next_line;
               p++;
            }
            while (*p && isspace((unsigned char)*p))
               p++;
            if (*p == ':' || *p == '=')
            {
               p++;
               return attn_parse_nonnegative_int(p, out);
            }
         }
      }

   next_line:
      while (*line && *line != '\n')
         line++;
      if (*line == '\n')
         line++;
   }

   return 0;
}

static int attn_read_file(const char *path, char **out)
{
   if (!path || !out)
      return 0;
   *out = NULL;

   FILE *f = fopen(path, "rb");
   if (!f)
      return 0;
   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      return 0;
   }
   long sz = ftell(f);
   if (fseek(f, 0, SEEK_SET) != 0)
   {
      fclose(f);
      return 0;
   }
   if (sz <= 0 || sz > (1 << 20))
   {
      fclose(f);
      return 0;
   }

   char *buf = (char *)malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return 0;
   }
   size_t got = fread(buf, 1, (size_t)sz, f);
   fclose(f);
   buf[got] = '\0';
   *out = buf;
   return 1;
}

static int attn_config_ingress_max_raw_scans(void)
{
   const char *home = aimee_home();
   if (!home || !home[0])
      return 0;

   char path[1024];
   snprintf(path, sizeof(path), "%s/aimee.yaml", home);

   char *buf = NULL;
   if (!attn_read_file(path, &buf))
      return 0;

   int value = 0;
   if (!attn_parse_ingress_max_raw_scans(buf, &value))
      value = 0;
   free(buf);
   return value;
}

int handle_attention_guard(void)
{
   const char *bypass = getenv("AIMEE_GUARD");
   if (bypass && strcmp(bypass, "0") == 0)
      return 0;

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

   if (op == ATTN_OP_RAW_SCAN)
   {
      int ingress_max_raw_scans = attn_config_ingress_max_raw_scans();
      int used = attn_raw_scan_count(arr, now_ts);
      if (ingress_max_raw_scans <= 0 || used >= ingress_max_raw_scans)
      {
         fprintf(stderr,
                 "aimee attention-guard: recursive raw scans are disabled or exhausted for this "
                 "context. "
                 "Use Aimee's indexed tools instead: find_symbol, ast_grep_search, "
                 "search_graph, or get_context_block. Set ingress_max_raw_scans above 0 "
                 "only when raw scanning is intentional.\n");
         exit_code = 2;
      }
      else
      {
         attn_record(arr, ATTN_RAW_SCAN_PATH, 1, now_ts);
      }
   }
   else if (op == ATTN_OP_HARD && bash_cmd && bash_cmd[0])
   {
      /* Block if the destructive command mentions a high-attention path. */
      attn_record_t *recs = (attn_record_t *)calloc(ATTN_MAX_RECORDS, sizeof(*recs));
      if (!recs)
      {
         cJSON_Delete(arr);
         cJSON_Delete(hook);
         free(stdin_data);
         return 0;
      }
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
      free(recs);
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
