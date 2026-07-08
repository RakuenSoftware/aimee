/* cli_attention_guard.c: see cli_attention_guard.h.
 *
 * Per-session attention log lives at $AIMEE_HOME/.cache/attention/<session>.json
 * as an array of {path, weight, ts}. On each PreToolUse the guard accrues the
 * current tool's attention (Read=2, edit-class=8) and, for a hard-destructive
 * Bash command, blocks (exit 2) when the command text mentions a path the log
 * scores at/above the high-attention threshold. Substring-matching known
 * high-attention paths against the command avoids fragile shell parsing.
 * The raw-scan redirect is OPT-IN: it is inert unless ingress_max_raw_scans is
 * set to a positive cap, in which case a session may run that many recursive
 * raw scans before further ones are redirected toward Aimee's indexed
 * exploration tools. With no cap configured (the default, and what a thin-client
 * host with no aimee.yaml sees) raw scans are never blocked. The guard has no
 * env-var off switch: disabling it is a deliberate operator config action
 * (require_session_worktree: false), never something the guarded agent can do.
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
#include <strings.h> /* strncasecmp */
#include <time.h>
#include <unistd.h> /* getcwd */

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

/* Parse a boolean `require_session_worktree:` line from an aimee.yaml buffer.
 * Accepts true/1/yes/on (case-insensitive) as true; anything else (incl. a
 * missing key) is false. Mirrors attn_parse_ingress_max_raw_scans' lean,
 * link-free YAML-line scan so the guard need not pull in the full config. */
static int attn_parse_require_session_worktree(const char *buf, int *out)
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

         size_t key_len = strlen("require_session_worktree");
         if (strncmp(p, "require_session_worktree", key_len) == 0)
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
               while (*p && (isspace((unsigned char)*p) || *p == '"' || *p == '\''))
                  p++;
               *out = (strncasecmp(p, "true", 4) == 0 || strncasecmp(p, "yes", 3) == 0 ||
                       strncasecmp(p, "on", 2) == 0 || *p == '1');
               return 1;
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

static int attn_config_require_session_worktree(void)
{
   /* Default ON: session-worktree isolation is required unless an operator
    * explicitly sets `require_session_worktree: false`. Two aimee sessions
    * sharing one checkout collide on a single git HEAD — one session's
    * `git checkout <branch>` moves the branch the other is mutating, cross-
    * contaminating commits. Failing closed to isolation (each session in its own
    * `.aimee/worktrees/...` worktree+branch) is the only safe default. */
   const char *home = aimee_home();
   if (!home || !home[0])
      return 1; /* no resolvable home -> fail closed to isolation */

   char path[1024];
   snprintf(path, sizeof(path), "%s/aimee.yaml", home);

   char *buf = NULL;
   if (!attn_read_file(path, &buf))
      return 1; /* no config file -> default ON */

   int value = 1; /* default ON; only an explicit key overrides */
   (void)attn_parse_require_session_worktree(buf, &value);
   free(buf);
   return value;
}

/* Lexically resolve '.', '..' and '//' in `path` into `out` (purely textual —
 * the write target may not exist yet, so realpath() is unusable; symlinks are
 * not followed). Closes the `.aimee/worktrees/../escape` traversal bypass: a
 * component sequence like ".../worktrees/x/../../src" collapses so the marker
 * substring no longer survives for an escaped path. A leading '/' is preserved;
 * '..' that would rise above the root is dropped (cannot escape '/'). */
static void attn_lexical_normalize(const char *path, char *out, size_t out_n)
{
   if (!out || out_n == 0)
      return;
   out[0] = '\0';
   if (!path || !path[0])
      return;

   int absolute = (path[0] == '/');
   /* Component start offsets within `out` form a simple stack. 256 components is
    * far beyond any real path; if exceeded, recording simply stops (a later '..'
    * may pop to a stale offset, yielding a shorter path) — this only ever drops
    * the worktree marker, i.e. fails closed (blocks), never opens a bypass. */
   size_t starts[256];
   int depth = 0;
   size_t len = 0;
   if (absolute && len + 1 < out_n)
      out[len++] = '/';

   const char *p = path;
   while (*p)
   {
      while (*p == '/')
         p++;
      if (!*p)
         break;
      const char *seg = p;
      while (*p && *p != '/')
         p++;
      size_t seglen = (size_t)(p - seg);

      if (seglen == 1 && seg[0] == '.')
         continue; /* current dir: skip */
      if (seglen == 2 && seg[0] == '.' && seg[1] == '.')
      {
         if (depth > 0)
         {
            depth--;
            len = starts[depth]; /* pop last component (and its '/') */
            if (len > 0 && out[len - 1] == '/' && !(absolute && len == 1))
               len--;
            out[len] = '\0';
         }
         /* else: '..' at/above root is dropped (absolute) or kept-as-root */
         continue;
      }

      /* Push a separator before this component when one is needed. */
      if (len > 0 && out[len - 1] != '/')
      {
         if (len + 1 >= out_n)
            break;
         out[len++] = '/';
      }
      if (depth < (int)(sizeof(starts) / sizeof(starts[0])))
         starts[depth++] = len;
      if (len + seglen >= out_n)
         break;
      memcpy(out + len, seg, seglen);
      len += seglen;
      out[len] = '\0';
   }
   if (len == 0 && out_n > 0)
   {
      out[0] = absolute ? '/' : '.';
      out[1] = '\0';
   }
}

/* Returns 1 iff `norm` (an already lexically-normalized path) is inside a
 * managed session worktree. Matches the canonical managed locations:
 *   "/.aimee/worktrees/"  — aimee's own launcher + delegate worktrees
 *   "/.claude/worktrees/" — Claude Code's native worktrees (EnterWorktree)
 *   "/.codex/worktrees/"  — Codex's native worktrees
 * All are isolated worktrees on a branch off the default branch — the exact
 * isolation this guard requires — so a Claude Code / Codex session working in its
 * own worktree is honoured, not blocked. Deliberately the full "/worktrees/" path,
 * NOT the looser "/.aimee-" / "/.claude" prefixes, which would false-match
 * unrelated dirs like "/home/u/.aimee-notes/..." or "~/.claude/". */
static int attn_path_in_managed_worktree(const char *norm)
{
   return norm && (strstr(norm, "/.aimee/worktrees/") != NULL ||
                   strstr(norm, "/.claude/worktrees/") != NULL ||
                   strstr(norm, "/.codex/worktrees/") != NULL);
}

/* Pure decision for the session-isolation guard (testable in isolation).
 * Returns 1 to BLOCK: a mutating op (SOFT/HARD) whose effective target is NOT
 * inside an aimee-managed worktree. Read / raw-scan ops are never blocked here.
 * The effective target is `file_path` when given (resolved against `cwd` when
 * relative), else `cwd` itself (a Bash mutation runs there). The joined path is
 * lexically normalized before the worktree check so a "../" escape out of the
 * worktree is blocked even though the raw string still contained the marker. */
int attn_session_isolation_blocked(attn_op_t op, const char *file_path, const char *cwd)
{
   if (op != ATTN_OP_SOFT && op != ATTN_OP_HARD)
      return 0;

   char joined[2048];
   if (file_path && file_path[0] == '/')
      snprintf(joined, sizeof(joined), "%s", file_path);
   else if (file_path && file_path[0])
      snprintf(joined, sizeof(joined), "%s/%s", cwd ? cwd : "", file_path);
   else
      snprintf(joined, sizeof(joined), "%s", cwd ? cwd : "");

   char norm[2048];
   attn_lexical_normalize(joined, norm, sizeof(norm));
   return attn_path_in_managed_worktree(norm) ? 0 : 1;
}

int handle_attention_guard(void)
{
   /* No env-var bypass. The guard must not be disableable by the agent it
    * guards — an LLM can trivially set an env var on any command, so a would-be
    * bypass has to be a deliberate OPERATOR action in config
    * (require_session_worktree: false / ingress_max_raw_scans), not an env var.
    * (Removed the former AIMEE_GUARD=0 escape hatch.) */
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

   /* Session-isolation guard (OPT-IN via require_session_worktree). Fails closed
    * on a mutating op outside an aimee-managed worktree, so a session that never
    * ran `session-start` (e.g. a missing SessionStart hook) cannot mutate the
    * primary checkout / default branch. This is the aimee-level backstop for the
    * worktree+branch isolation that the SessionStart hook would otherwise set up. */
   if ((op == ATTN_OP_SOFT || op == ATTN_OP_HARD) && attn_config_require_session_worktree())
   {
      char cwd[1024];
      if (!getcwd(cwd, sizeof(cwd)))
         cwd[0] = '\0';
      const char *fp =
          ti ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(ti, "file_path")) : NULL;
      if (attn_session_isolation_blocked(op, fp, cwd))
      {
         fprintf(stderr,
                 "aimee attention-guard: refusing a mutating op outside a session worktree. "
                 "This session is operating in '%s', which is not a managed worktree "
                 "(.aimee/worktrees/... or .claude/worktrees/...). aimee requires each mutating "
                 "session to run in an isolated worktree+branch off the default branch: create "
                 "one (Claude Code: EnterWorktree; aimee: launch `aimee`, which materializes a "
                 "worktree and chdirs into it). An operator may set require_session_worktree: "
                 "false in aimee.yaml to disable this — there is no env-var bypass.\n",
                 cwd[0] ? cwd : "(unknown cwd)");
         cJSON_Delete(hook);
         free(stdin_data);
         return 2;
      }
   }

   char path[1024];
   attn_log_path(sid, path, sizeof(path));
   cJSON *arr = attn_load(path);

   int exit_code = 0;

   if (op == ATTN_OP_RAW_SCAN)
   {
      /* The raw-scan cap is OPT-IN: ingress_max_raw_scans <= 0 (the default,
       * and what a thin-client host with no aimee.yaml sees) means the guard is
       * disabled, so raw scans flow freely. Only a positive cap is enforced —
       * after that many scans this session, redirect to the indexed tools. */
      int ingress_max_raw_scans = attn_config_ingress_max_raw_scans();
      if (ingress_max_raw_scans > 0)
      {
         int used = attn_raw_scan_count(arr, now_ts);
         if (used >= ingress_max_raw_scans)
         {
            fprintf(stderr,
                    "aimee attention-guard: this session has hit its raw-scan cap (%d). Aimee "
                    "indexes this repo — explore through it instead: find_symbol, "
                    "ast_grep_search, search_graph, or get_context_block. Raise "
                    "ingress_max_raw_scans in aimee.yaml to raise or lift the cap.\n",
                    ingress_max_raw_scans);
            exit_code = 2;
         }
         else
         {
            attn_record(arr, ATTN_RAW_SCAN_PATH, 1, now_ts);
         }
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
