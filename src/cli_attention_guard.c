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
#include "agent_code_capabilities.h"
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
#include <sys/stat.h> /* stat, S_ISDIR — telling a file target from a directory */
#include <unistd.h>   /* getcwd */

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

/* Parse a boolean `<key>:` line from an aimee.yaml buffer. Accepts
 * true/1/yes/on (case-insensitive) as true; anything else (incl. a missing
 * key) leaves *out untouched. Mirrors attn_parse_ingress_max_raw_scans' lean,
 * link-free YAML-line scan so the guard need not pull in the full config. */
static int attn_parse_bool_key(const char *buf, const char *key, int *out)
{
   if (!buf || !key || !out)
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

         size_t key_len = strlen(key);
         if (strncmp(p, key, key_len) == 0)
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
   (void)attn_parse_bool_key(buf, "require_session_worktree", &value);
   free(buf);
   return value;
}

static int attn_config_require_aimee_memory(void)
{
   /* Default ON: durable memories authored by an agent belong in aimee's memory
    * system (`aimee memory store`), where they are indexed, recalled, and
    * audited — not in per-harness markdown files aimee never sees. Only an
    * explicit `require_aimee_memory: false` opts out. */
   const char *home = aimee_home();
   if (!home || !home[0])
      return 1; /* no resolvable home -> fail closed to enforcement */

   char path[1024];
   snprintf(path, sizeof(path), "%s/aimee.yaml", home);

   char *buf = NULL;
   if (!attn_read_file(path, &buf))
      return 1; /* no config file -> default ON */

   int value = 1; /* default ON; only an explicit key overrides */
   (void)attn_parse_bool_key(buf, "require_aimee_memory", &value);
   free(buf);
   return value;
}

/* Public wrapper so other client TUs (e.g. the remote/thin session-start path)
 * can ask the SAME authoritative question the guard uses to decide whether to
 * block — default ON unless aimee.yaml sets `require_session_worktree: false`. */
int attn_require_session_worktree(void)
{
   return attn_config_require_session_worktree();
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

/* Returns 1 iff `norm` is harness-owned session state rather than repo content:
 * Claude Code's per-project state dir ("~/.claude/projects/<slug>/..." —
 * transcripts, tool-result spills). The harness writes there as part of normal
 * operation from ANY cwd; blocking it doesn't protect the checkout, it just
 * breaks the harness. The loose "/.claude/" prefix stays blocked — only the
 * projects state dir is carved out. NOTE: the file-based agent-memory subtree
 * of this dir is NOT writable through this carve-out — the external-memory
 * guard (attn_external_memory_blocked, checked first) redirects memory writes
 * into aimee's memory system. */
static int attn_path_is_harness_state(const char *norm)
{
   return norm && strstr(norm, "/.claude/projects/") != NULL;
}

/* Copies the next '/'-delimited component of `*p` into `out` and advances `*p`
 * past it. Returns 0 when there is no component left or it does not fit. */
static int attn_next_component(const char **p, char *out, size_t outsz)
{
   const char *s = *p;
   if (*s != '/')
      return 0;
   s++;
   const char *end = strchr(s, '/');
   size_t len = end ? (size_t)(end - s) : strlen(s);
   if (len == 0 || len >= outsz)
      return 0;
   memcpy(out, s, len);
   out[len] = '\0';
   *p = end ? end : s + len;
   return 1;
}

/* Resolves the deepest EXISTING ancestor of `path` through symlinks. The tail
 * that does not exist yet cannot contain a symlink, so resolving the existing
 * prefix is enough to decide containment for a not-yet-created file. */
static int attn_resolve_existing_ancestor(const char *path, char *out, size_t outsz)
{
   char buf[2048];
   snprintf(buf, sizeof(buf), "%s", path);
   for (;;)
   {
      char *r = realpath(buf, NULL);
      if (r)
      {
         int fits = snprintf(out, outsz, "%s", r) >= 0 && strlen(r) < outsz;
         free(r);
         return fits;
      }
      char *slash = strrchr(buf, '/');
      if (!slash || slash == buf)
         return 0;
      *slash = '\0';
   }
}

/* Returns 1 iff `norm` is inside THIS session's harness scratch directory:
 * "<tmp>/claude-<uid>/<project-slug>/<session-id>/..." (scratchpad, task output,
 * tool-result spills). Same rationale as the projects state dir above — it is
 * harness-owned session scratch, not repo content, so blocking it protects no
 * checkout. It also blocked only the honest path: a Bash heredoc to the same
 * file was never a SOFT/HARD op, so the guard stopped Write/Edit and waved the
 * shell through, which is worse than allowing both.
 *
 * Bound to `session_id`, so the carve-out admits the scratch dir the harness
 * gave THIS session and nothing else — never a general "/tmp is writable" hole,
 * and never another session's scratch. An absent session id means no carve-out.
 *
 * Two things this deliberately does NOT do loosely, both because a carve-out in
 * a fail-closed control is exactly where a hole would hide:
 *
 *   - The LAYOUT is positional, not a set of substring probes. An earlier
 *     revision accepted any temp path that contained "/claude-" somewhere and
 *     the session id as some component, which admitted unrelated shapes like
 *     "<tmp>/unrelated/claude-marker/<session-id>/checkout/x". The components
 *     must now be exactly <claude-*>/<slug>/<session-id> directly beneath the
 *     temp root.
 *
 *   - CONTAINMENT is filesystem-resolved, not lexical. Normalization stops
 *     "../" but not a symlink: "<scratch>/link/src/x.c" where "link" points at
 *     the primary checkout is lexically inside scratch and physically not. Both
 *     the scratch root and the target's deepest existing ancestor are resolved
 *     through symlinks and the latter must still lie under the former — AND the
 *     resolved root must still carry the expected layout beneath the resolved
 *     temp dir, or a session directory that is ITSELF a symlink into the
 *     checkout would land both sides inside the checkout and pass trivially.
 *
 * WHAT THIS DOES NOT DO, stated rather than left implied: the resolution is a
 * PREFLIGHT. A PreToolUse hook advises; the harness performs the write after the
 * hook returns, so a directory checked here can in principle be swapped for a
 * symlink before that write. Closing that needs no-follow / beneath semantics
 * bound to the eventual open, which this process does not perform and cannot
 * impose on the harness.
 *
 * That limit is not specific to this carve-out and is not the weakest link:
 * attn_path_in_managed_worktree — the rule that admits ordinary work — is purely
 * LEXICAL, with no resolution at all, so a symlink inside a worktree pointing at
 * the primary checkout already passes it. This path is strictly stronger than
 * the control it sits beside. Removing the carve-out would restore the original
 * defect (file tools blocked while an equivalent shell heredoc passes) without
 * closing a race that exists independently of it. */
static int attn_path_is_session_scratch(const char *norm, const char *session_id)
{
   if (!norm || !session_id || !session_id[0])
      return 0;

   const char *tmpdir = getenv("TMPDIR");
   if (!tmpdir || !tmpdir[0] || tmpdir[0] != '/')
      tmpdir = "/tmp";
   size_t tlen = strlen(tmpdir);
   while (tlen > 1 && tmpdir[tlen - 1] == '/')
      tlen--;
   if (strncmp(norm, tmpdir, tlen) != 0 || norm[tlen] != '/')
      return 0;

   /* Exactly <tmp>/claude-<uid>/<project-slug>/<session-id>[/...]. */
   const char *p = norm + tlen;
   char comp[256];
   if (!attn_next_component(&p, comp, sizeof(comp)))
      return 0;
   if (strncmp(comp, "claude-", 7) != 0 || !comp[7])
      return 0;
   if (!attn_next_component(&p, comp, sizeof(comp))) /* project slug */
      return 0;
   if (!attn_next_component(&p, comp, sizeof(comp)) || strcmp(comp, session_id) != 0)
      return 0;

   /* Physical containment: the lexical shape is right, now prove the target
    * does not leave the session dir through a symlink. */
   char root[2048];
   size_t rootlen = (size_t)(p - norm);
   if (rootlen >= sizeof(root))
      return 0;
   memcpy(root, norm, rootlen);
   root[rootlen] = '\0';

   char root_real[2048], target_real[2048];
   if (!attn_resolve_existing_ancestor(root, root_real, sizeof(root_real)))
      return 0; /* the session dir must actually exist to be carved out */

   /* THE ROOT ITSELF MUST BE GENUINE SCRATCH. Comparing the target against the
    * resolved root proves the target does not escape the root — it says nothing
    * about where the ROOT went. If the session directory is itself a symlink into
    * the primary checkout, both sides resolve inside the checkout, the prefix
    * test below passes, and the carve-out authorises writing to the repository.
    * So the resolved root must still carry the expected claude-<uid> / slug /
    * session-id shape beneath the RESOLVED temp dir: same layout, no
    * redirection anywhere along it. */
   char tmp_real[2048];
   if (!attn_resolve_existing_ancestor(tmpdir, tmp_real, sizeof(tmp_real)))
      return 0;
   size_t tl = strlen(tmp_real);
   if (strncmp(root_real, tmp_real, tl) != 0 || root_real[tl] != '/')
      return 0;
   {
      const char *q = root_real + tl;
      char c2[256];
      if (!attn_next_component(&q, c2, sizeof(c2)))
         return 0;
      if (strncmp(c2, "claude-", 7) != 0 || !c2[7])
         return 0;
      if (!attn_next_component(&q, c2, sizeof(c2))) /* project slug */
         return 0;
      if (!attn_next_component(&q, c2, sizeof(c2)) || strcmp(c2, session_id) != 0)
         return 0;
      if (*q != '\0') /* nothing may follow the session dir */
         return 0;
   }

   if (!attn_resolve_existing_ancestor(norm, target_real, sizeof(target_real)))
      return 0;
   size_t rl = strlen(root_real);
   if (strncmp(target_real, root_real, rl) != 0)
      return 0;
   return target_real[rl] == '\0' || target_real[rl] == '/';
}

/* Returns 1 iff `norm` points into an external file-based agent-memory store:
 * Claude Code's per-project auto-memory dir
 * ("~/.claude/projects/<slug>/memory/..." incl. its MEMORY.md index). Matches
 * the "/memory" component itself and anything under it. */
static int attn_path_is_external_agent_memory(const char *norm)
{
   const char *m = norm ? strstr(norm, "/.claude/projects/") : NULL;
   if (!m)
      return 0;
   const char *mem = strstr(m + 18, "/memory"); /* 18 = strlen("/.claude/projects/") */
   if (!mem)
      return 0;
   return mem[7] == '\0' || mem[7] == '/';
}

/* True when `cmd` contains an output redirect that actually lands somewhere
 * ('>' / '>>'), ignoring pure-discard ("> /dev/null") and fd-dup ("2>&1")
 * forms so a read like `cat f 2>/dev/null` is not misread as a write. */
static int cmd_has_real_redirect(const char *cmd)
{
   for (const char *p = cmd ? strchr(cmd, '>') : NULL; p; p = strchr(p + 1, '>'))
   {
      const char *q = p + 1;
      if (*q == '>')
         q++;
      if (*q == '&')
      {
         q++;
         if (isdigit((unsigned char)*q))
            continue; /* fd dup, e.g. 2>&1 */
      }
      while (*q == ' ' || *q == '\t')
         q++;
      if (strncmp(q, "/dev/null", 9) == 0)
         continue;
      return 1;
   }
   return 0;
}

/* True when `tok` appears in `cmd` at a word START (preceded by whitespace,
 * start-of-string, or a shell separator) — the right side is unconstrained so
 * "python" matches "python3" and "node" matches "nodejs". Over-matching is the
 * fail-closed direction here. */
static int cmd_has_word_prefix(const char *cmd, const char *tok)
{
   size_t n = strlen(tok);
   for (const char *p = cmd; (p = strstr(p, tok)) != NULL; p += n)
   {
      char l = (p == cmd) ? ' ' : p[-1];
      if (l == ' ' || l == '\t' || l == '\n' || l == ';' || l == '|' || l == '&' || l == '(' ||
          l == '`' || l == '$')
         return 1;
   }
   return 0;
}

/* True if `cmd` plausibly writes files: a real output redirect, an in-place
 * editor, a file-moving tool, or an interpreter (which can write anything).
 * Conservative substring matching, same spirit as bash_is_hard(): a false
 * positive blocks with a clear redirect message; a false negative would let a
 * memory write slip through. */
static int bash_cmd_can_write(const char *cmd)
{
   if (!cmd || !cmd[0])
      return 0;
   if (cmd_has_real_redirect(cmd))
      return 1;
   static const char *toks[] = {
       "tee",  "sed -i", "sed --in-place", "perl -i", "awk -i",  "cp",    "mv",
       "rm",   "touch",  "truncate",       "shred",   "install", "rsync", "dd",
       "ln",   "chmod",  "chown",          "mkdir",   "python",  "perl",  "ruby",
       "node", "php",    "xargs",          "sh -c",   "bash -c", NULL};
   for (int i = 0; toks[i]; i++)
      if (strchr(toks[i], ' ') ? strstr(cmd, toks[i]) != NULL : cmd_has_word_prefix(cmd, toks[i]))
         return 1;
   return 0;
}

/* Does a writing Bash command reach OUTSIDE every managed worktree?
 *
 * The isolation check judges the tool's file_path, or the cwd when there isn't one. For a
 * Bash call that means it judges WHERE THE COMMAND STARTS, not what it touches -- so
 * `cd /repo && python3 - <<EOF ...` passes while writing anywhere on disk. That is not
 * hypothetical: it is how this guard was bypassed, from a session whose cwd was a
 * perfectly good worktree.
 *
 * Two escape shapes are detected, both cheap and both literal:
 *   1. `cd <abs>` / `pushd <abs>` to a directory outside a managed worktree -- the
 *      observed bypass, and the one that relocates everything after it.
 *   2. a redirect whose target is an absolute path outside a managed worktree.
 *
 * Only consulted when bash_cmd_can_write() already says the command mutates, so a
 * read-only command naming absolute paths is untouched.
 *
 * LIMITS, stated plainly rather than implied: this does not parse shell. A path built
 * from a variable, passed through an interpreter's own logic, or reached via a symlink
 * will not be seen. It is defence in depth against the common and observed shapes, not a
 * sandbox -- a determined agent can still get out, and the durable fix is executing tool
 * calls under an OS-level write confinement rather than pattern-matching their text. */
int attn_bash_escapes_worktree(const char *bash_cmd, const char *cwd)
{
   if (!bash_cmd || !bash_cmd[0])
      return 0;
   if (!bash_cmd_can_write(bash_cmd))
      return 0;

   /* ---- 1. cd / pushd to an absolute path ---- */
   static const char *const chdirs[] = {"cd ", "pushd ", NULL};
   for (int i = 0; chdirs[i]; i++)
   {
      const char *p = bash_cmd;
      size_t toklen = strlen(chdirs[i]);
      while ((p = strstr(p, chdirs[i])) != NULL)
      {
         /* Must start a command: beginning of string, or after ; && || | newline. */
         int at_cmd_start = (p == bash_cmd);
         if (!at_cmd_start)
         {
            const char *b = p - 1;
            while (b > bash_cmd && (*b == ' ' || *b == '\t'))
               b--;
            at_cmd_start = (*b == ';' || *b == '&' || *b == '|' || *b == '\n' || *b == '(');
         }
         const char *arg = p + toklen;
         p = arg;
         if (!at_cmd_start)
            continue;
         while (*arg == ' ' || *arg == '\t' || *arg == '\'' || *arg == '"')
            arg++;
         if (*arg != '/')
            continue; /* relative cd stays within the worktree by construction */
         char path[2048];
         size_t n = 0;
         while (arg[n] && n + 1 < sizeof(path) && arg[n] != ' ' && arg[n] != '\t' &&
                arg[n] != ';' && arg[n] != '&' && arg[n] != '|' && arg[n] != '\'' &&
                arg[n] != '"' && arg[n] != '\n')
         {
            path[n] = arg[n];
            n++;
         }
         path[n] = '\0';
         char norm[2048];
         attn_lexical_normalize(path, norm, sizeof(norm));
         if (!attn_path_in_managed_worktree(norm))
            return 1;
      }
   }

   /* ---- 2. redirect target that is an absolute path ---- */
   for (const char *q = strchr(bash_cmd, '>'); q; q = strchr(q + 1, '>'))
   {
      const char *t = q + 1;
      if (*t == '>')
         t++;
      if (*t == '&')
         continue; /* fd dup */
      while (*t == ' ' || *t == '\t' || *t == '\'' || *t == '"')
         t++;
      if (*t != '/')
         continue;
      if (strncmp(t, "/dev/null", 9) == 0)
         continue;
      char path[2048];
      size_t n = 0;
      while (t[n] && n + 1 < sizeof(path) && t[n] != ' ' && t[n] != '\t' && t[n] != ';' &&
             t[n] != '&' && t[n] != '|' && t[n] != '\'' && t[n] != '"' && t[n] != '\n')
      {
         path[n] = t[n];
         n++;
      }
      path[n] = '\0';
      char norm[2048];
      attn_lexical_normalize(path, norm, sizeof(norm));
      if (!attn_path_in_managed_worktree(norm) && !attn_path_is_harness_state(norm))
         return 1;
   }

   /* ---- 3. absolute args to commands whose arguments ARE the write target ----
    * Only verbs where every path argument is written: tee, truncate, touch, mkdir, rm,
    * shred, install, sed -i. Deliberately NOT cp/mv/rsync -- their first arguments are
    * SOURCES, and reading an outside file to write inside the worktree is legitimate. */
   static const char *const target_verbs[] = {"tee", "truncate", "touch",   "mkdir",
                                              "rm",  "shred",    "install", NULL};
   for (int i = 0; target_verbs[i]; i++)
   {
      if (!cmd_has_word_prefix(bash_cmd, target_verbs[i]) && !strstr(bash_cmd, target_verbs[i]))
         continue;
      const char *p = strstr(bash_cmd, target_verbs[i]);
      while (p)
      {
         /* Scan this segment's arguments up to the next command separator. */
         const char *a = p + strlen(target_verbs[i]);
         while (*a && *a != ';' && *a != '|' && *a != '\n' && !(*a == '&' && a[1] == '&'))
         {
            while (*a == ' ' || *a == '\t' || *a == '\'' || *a == '"')
               a++;
            if (*a == '/')
            {
               char path[2048];
               size_t n = 0;
               while (a[n] && n + 1 < sizeof(path) && a[n] != ' ' && a[n] != '\t' && a[n] != ';' &&
                      a[n] != '&' && a[n] != '|' && a[n] != '\'' && a[n] != '"' && a[n] != '\n')
               {
                  path[n] = a[n];
                  n++;
               }
               path[n] = '\0';
               char norm[2048];
               attn_lexical_normalize(path, norm, sizeof(norm));
               if (strncmp(norm, "/dev/null", 9) != 0 && !attn_path_in_managed_worktree(norm) &&
                   !attn_path_is_harness_state(norm))
                  return 1;
               a += n;
               continue;
            }
            /* skip a non-absolute token */
            while (*a && *a != ' ' && *a != '\t' && *a != ';' && *a != '|' && *a != '\n' &&
                   !(*a == '&' && a[1] == '&'))
               a++;
         }
         p = strstr(p + 1, target_verbs[i]);
      }
   }

   (void)cwd;
   return 0;
}

/* External-memory decision (pure, testable). Returns 1 to BLOCK a tool call
 * that would WRITE the harness's file-based agent-memory store — directly
 * (a mutating file tool whose target resolves into the store) or via a Bash
 * command that names the store with write intent (the observed bypass:
 * `cat > .../memory/x.md`, `sed -i`, `python3 - <<EOF`). Reads stay free.
 * For Bash the RAW command text is scanned — the path may be smuggled through
 * a variable, but the literal store path has to appear somewhere for the
 * command to reach it in a fresh shell. */
int attn_external_memory_blocked(attn_op_t op, const char *tool_name, const char *file_path,
                                 const char *bash_cmd, const char *cwd)
{
   if (tool_name && strcmp(tool_name, "Bash") == 0)
   {
      const char *m = bash_cmd ? strstr(bash_cmd, ".claude/projects/") : NULL;
      if (!m || !strstr(m, "memory"))
         return 0;
      return bash_cmd_can_write(bash_cmd);
   }

   /* File-target tools: only mutating ops (Write/Edit/NotebookEdit classify
    * SOFT; Read never blocks). */
   if (op != ATTN_OP_SOFT && op != ATTN_OP_HARD)
      return 0;
   if (!file_path || !file_path[0])
      return 0;

   char joined[2048];
   if (file_path[0] == '/')
      snprintf(joined, sizeof(joined), "%s", file_path);
   else
      snprintf(joined, sizeof(joined), "%s/%s", cwd ? cwd : "", file_path);
   char norm[2048];
   attn_lexical_normalize(joined, norm, sizeof(norm));
   return attn_path_is_external_agent_memory(norm);
}

/* ---- session BRANCH lineage enforcement ------------------------------------
 *
 * attn_path_in_managed_worktree above checks only that a path sits under a managed
 * worktree directory. Its comment claimed those are "isolated worktrees on a branch off
 * the default branch" -- but nothing verified the branch, so a worktree created by hand
 * (`git worktree add -b x <arbitrary-commit>`) under .claude/worktrees/ satisfied the
 * guard while being rooted on another session's branch. That is how a session ended up
 * 115 commits behind the default branch, carrying ~18 commits of another agent's
 * unmerged work it could not separate from its own.
 *
 * The rule being enforced:
 *   PRIMARY  session branch must be cut from the DEFAULT branch.
 *   DELEGATE session branch may be cut from its parent primary's branch.
 *
 * The base ref is read from the worktree registry, written by the launcher at creation
 * time (workspace.c). It is deliberately NOT taken from the environment: this guard
 * refuses env-var inputs on principle, because an LLM can set AIMEE_DELEGATE_DEPTH on
 * any command and would simply declare itself a delegate to escape the primary rule.
 * The registry is written by trusted code before the agent runs. */

/* Pure decision (testable): 1 = BLOCK.
 *  base_branch      the ref this worktree was cut from, "" if unknown/unregistered
 *  default_branch   the repo's default branch, "" if unresolvable
 *  base_is_registered  1 iff base_branch is itself a registered session branch,
 *                      i.e. a legitimate delegate parent
 * Unknown base blocks: a worktree with no registry row was not created by the launcher,
 * which is exactly the hand-rolled case this exists to catch. Fail closed. */
int attn_session_branch_blocked(const char *base_branch, const char *default_branch,
                                int base_is_registered)
{
   if (!base_branch || !base_branch[0])
      return 1; /* unregistered worktree -> not launcher-created -> block */
   if (default_branch && default_branch[0] && strcmp(base_branch, default_branch) == 0)
      return 0; /* primary rooted on the default branch */
   /* Accept both "main" and "origin/main" spellings of the same default. */
   if (default_branch && default_branch[0])
   {
      const char *slash = strrchr(default_branch, '/');
      const char *bare = slash ? slash + 1 : default_branch;
      const char *bslash = strrchr(base_branch, '/');
      const char *bbare = bslash ? bslash + 1 : base_branch;
      if (strcmp(bbare, bare) == 0)
         return 0;
   }
   if (base_is_registered)
      return 0; /* delegate rooted on a real parent session branch */
   return 1;
}

/* Resolve the repo's default branch for the checkout containing `norm`, e.g. "testing".
 * Mirrors code_collect.c's chain: origin/HEAD, then the usual candidates. Returns the
 * bare branch name (no "origin/" prefix) or "" when unresolvable. */
static void attn_resolve_default_branch(const char *norm, char *out, size_t outlen)
{
   if (out && outlen)
      out[0] = '\0';
   if (!norm || !norm[0] || !out || !outlen)
      return;
   char cmd[2400];
   snprintf(cmd, sizeof(cmd),
            "git -C '%s' symbolic-ref --short refs/remotes/origin/HEAD 2>/dev/null", norm);
   FILE *fp2 = popen(cmd, "r");
   if (fp2)
   {
      if (fgets(out, (int)outlen, fp2))
      {
         size_t n = strlen(out);
         while (n && (out[n - 1] == '\n' || out[n - 1] == '\r'))
            out[--n] = '\0';
      }
      pclose(fp2);
   }
   if (out[0])
   {
      const char *slash = strrchr(out, '/');
      if (slash)
         memmove(out, slash + 1, strlen(slash + 1) + 1);
      return;
   }
   snprintf(out, outlen, "%s", "main");
}

/* Read the launcher-written registry row for the worktree containing `norm`.
 *
 * Row format (workspace.c): repo \t worktree \t branch \t session_id \t work_name \t base.
 * Returns 1 and fills base_out when a row's worktree path is a prefix of `norm`; 0 when
 * no row matches, which the caller treats as "not launcher-created" and blocks.
 *
 * Reads the registry, never the environment: this guard refuses env-var inputs because
 * the agent it guards can set any of them. */
/* Returns: 2 = row found for this worktree, 1 = a registry exists but has no row for it,
 * 0 = no registry file at all (repo has never used managed worktrees). */
static int attn_registry_base_for(const char *norm, char *base_out, size_t base_len,
                                  char *branch_out, size_t branch_len)
{
   if (base_out && base_len)
      base_out[0] = '\0';
   if (branch_out && branch_len)
      branch_out[0] = '\0';
   if (!norm || !norm[0])
      return 0;

   /* The registry lives at <repo>/.aimee/worktrees/registry.tsv. Walk up from the
    * target until one is found, so this works from any depth inside a worktree. */
   int saw_registry = 0;
   char probe[2048];
   snprintf(probe, sizeof(probe), "%s", norm);
   for (int depth = 0; depth < 40; depth++)
   {
      char reg[2200];
      snprintf(reg, sizeof(reg), "%s/.aimee/worktrees/registry.tsv", probe);
      char *buf = NULL;
      if (attn_read_file(reg, &buf) && buf)
      {
         int found = 0;
         char *line = buf;
         while (*line)
         {
            char *nl = strchr(line, '\n');
            if (nl)
               *nl = '\0';
            /* fields: repo, worktree, branch, sid, work_name, base */
            char *f[6] = {0};
            int nf = 0;
            char *p2 = line;
            f[nf++] = p2;
            while (nf < 6 && (p2 = strchr(p2, '\t')))
            {
               *p2++ = '\0';
               f[nf++] = p2;
            }
            if (nf >= 2 && f[1] && f[1][0])
            {
               size_t wl = strlen(f[1]);
               /* worktree path is a prefix of the target (exact dir or below it) */
               if (strncmp(norm, f[1], wl) == 0 && (norm[wl] == '\0' || norm[wl] == '/'))
               {
                  if (base_out && base_len && nf >= 6 && f[5])
                     snprintf(base_out, base_len, "%s", f[5]);
                  if (branch_out && branch_len && nf >= 3 && f[2])
                     snprintf(branch_out, branch_len, "%s", f[2]);
                  found = 1;
               }
            }
            if (!nl)
               break;
            line = nl + 1;
         }
         free(buf);
         if (found)
            return 2;
         saw_registry = 1;
      }
      char *slash = strrchr(probe, '/');
      if (!slash || slash == probe)
         break;
      *slash = '\0';
   }
   return saw_registry ? 1 : 0;
}

/* 1 iff `branch` appears as a registered session branch (column 3) anywhere in the
 * registry reachable from `norm` -- i.e. a legitimate delegate parent. */
static int attn_registry_branch_is_registered(const char *norm, const char *branch)
{
   if (!branch || !branch[0] || !norm || !norm[0])
      return 0;
   char probe[2048];
   snprintf(probe, sizeof(probe), "%s", norm);
   for (int depth = 0; depth < 40; depth++)
   {
      char reg[2200];
      snprintf(reg, sizeof(reg), "%s/.aimee/worktrees/registry.tsv", probe);
      char *buf = NULL;
      if (attn_read_file(reg, &buf) && buf)
      {
         int hit = 0;
         char *line = buf;
         while (*line && !hit)
         {
            char *nl = strchr(line, '\n');
            if (nl)
               *nl = '\0';
            char *t1 = strchr(line, '\t');
            char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
            char *t3 = t2 ? strchr(t2 + 1, '\t') : NULL;
            if (t2 && t3)
            {
               *t3 = '\0';
               if (strcmp(t2 + 1, branch) == 0)
                  hit = 1;
            }
            if (!nl)
               break;
            line = nl + 1;
         }
         free(buf);
         if (hit)
            return 1;
      }
      char *slash = strrchr(probe, '/');
      if (!slash || slash == probe)
         break;
      *slash = '\0';
   }
   return 0;
}

/* Pure decision (testable): 1 = BLOCK, for a worktree under the managed root that has
 * NO registry row.
 *
 * Blocking those outright was wrong. The registry is written by ONE launcher
 * (workspace.c), while this guard's own refusal text names Claude Code's EnterWorktree
 * as a sanctioned launcher too -- and that one writes no row. So every EnterWorktree
 * session in an aimee repo was refused every mutating op, while sitting on a branch cut
 * from the default branch: exactly what the rule wants to allow. Worse, there was no way
 * out. Correcting it means writing settings, the binary, or aimee.yaml -- all outside a
 * managed worktree, all refused by this same guard, including the documented
 * `require_session_worktree: false` escape hatch, which lives in the very file it will
 * not let you write.
 *
 * With no row to read, provenance comes from git rather than from trust. What the
 * lineage rule actually exists to catch is a branch carrying another session's unmerged
 * work (the incident: 115 commits behind default, ~18 of another agent's). That is
 * decidable from commit topology: if this branch shares a commit with another registered
 * session branch that the default branch does not already contain, it was cut from that
 * session. `shares_foreign_session_history` carries that answer.
 *
 * Unlike an env var, this cannot simply be asserted -- an agent would have to rewrite
 * history to defeat it, which is the property the registry was chosen for.
 *
 * `default_resolved` is 0 when the default branch could not be determined; there is then
 * nothing to measure lineage against, so fail closed exactly as the registry path does. */
int attn_unregistered_lineage_blocked(int default_resolved, int shares_foreign_session_history)
{
   if (!default_resolved)
      return 1;
   return shares_foreign_session_history ? 1 : 0;
}

/* The branch checked out in the worktree at `dir`, or "" when git cannot say. A detached
 * HEAD reports "HEAD", normalized to "". Only needed on the no-registry-row path, which
 * has no column 3 to read it from. */
static void attn_git_current_branch(const char *dir, char *out, size_t outlen)
{
   if (out && outlen)
      out[0] = '\0';
   if (!dir || !dir[0] || !out || !outlen)
      return;
   char cmd[2400];
   snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse --abbrev-ref HEAD 2>/dev/null", dir);
   FILE *fp = popen(cmd, "r");
   if (!fp)
      return;
   if (fgets(out, (int)outlen, fp))
   {
      size_t n = strlen(out);
      while (n && (out[n - 1] == '\n' || out[n - 1] == '\r'))
         out[--n] = '\0';
   }
   pclose(fp);
   if (strcmp(out, "HEAD") == 0)
      out[0] = '\0';
}

/* A ref that actually resolves for the default branch: origin/<branch> when present,
 * else the local branch, since a fresh worktree often carries only the remote-tracking
 * ref. 0 when neither resolves -- the caller treats that as unresolved and fails closed.
 *
 * When both resolve, the one that already contains the other wins. This ref is the
 * yardstick for lineage, and a local default branch goes stale the moment it is not
 * pulled. On a box whose local `testing` sat 2542 commits behind origin/testing, every
 * session branch cut from the CURRENT default shared a merge base the stale local ref
 * did not contain, so attn_git_shares_foreign_session_history() flagged all of them and
 * refused every mutating op -- including the write of the `require_session_worktree:
 * false` hatch that is the documented way out. Preferring the containing ref also keeps
 * a local default carrying unpushed commits from being discarded in favour of a remote
 * that lacks them. */
static int attn_git_ref_exists(const char *dir, const char *ref)
{
   char cmd[2600];
   snprintf(cmd, sizeof(cmd),
            "git -C '%s' rev-parse --verify --quiet '%s^{commit}' >/dev/null 2>&1", dir, ref);
   return system(cmd) == 0;
}

static int attn_git_default_ref(const char *dir, const char *defbr, char *out, size_t outlen)
{
   if (!dir || !dir[0] || !defbr || !defbr[0] || !out || !outlen)
      return 0;
   if (strchr(defbr, '\'') || strchr(defbr, '\n'))
      return 0;
   char remote[300];
   snprintf(remote, sizeof(remote), "origin/%s", defbr);

   int has_local = attn_git_ref_exists(dir, defbr);
   int has_remote = attn_git_ref_exists(dir, remote);
   if (!has_local && !has_remote)
      return 0;
   if (has_local && !has_remote)
   {
      snprintf(out, outlen, "%s", defbr);
      return 1;
   }
   if (has_remote && !has_local)
   {
      snprintf(out, outlen, "%s", remote);
      return 1;
   }

   /* Both resolve: take whichever already contains the other, so neither a local
    * branch left behind nor one carrying unpushed default-branch commits can
    * shrink the yardstick. Ties (identical refs) fall to the local name. */
   char cmd[2600];
   snprintf(cmd, sizeof(cmd),
            "git -C '%s' merge-base --is-ancestor '%s^{commit}' '%s^{commit}' >/dev/null 2>&1", dir,
            remote, defbr);
   int local_contains_remote = system(cmd) == 0;
   snprintf(out, outlen, "%s", local_contains_remote ? defbr : remote);
   return 1;
}

/* 1 iff the worktree's HEAD shares unmerged history with a registered session branch
 * other than its own -- i.e. it was cut from that session rather than from the default
 * branch. For each registered branch S, the merge base of HEAD and S, when NOT already
 * contained in the default branch, is a commit the two sessions share and the default
 * branch lacks. That is the signature of the incident this rule exists for.
 *
 * A branch merely BEHIND the default branch shares no such commit and is allowed: being
 * stale is not the same as being rooted somewhere it should not be. */
static int attn_git_shares_foreign_session_history(const char *dir, const char *defref,
                                                   const char *own)
{
   char probe[2048];
   snprintf(probe, sizeof(probe), "%s", dir);
   for (int depth = 0; depth < 40; depth++)
   {
      char reg[2200];
      snprintf(reg, sizeof(reg), "%s/.aimee/worktrees/registry.tsv", probe);
      char *buf = NULL;
      if (attn_read_file(reg, &buf) && buf)
      {
         int shared = 0;
         char *line = buf;
         while (*line && !shared)
         {
            char *nl = strchr(line, '\n');
            if (nl)
               *nl = '\0';
            char *t1 = strchr(line, '\t');
            char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
            char *t3 = t2 ? strchr(t2 + 1, '\t') : NULL;
            if (t2 && t3)
            {
               *t3 = '\0';
               const char *sess = t2 + 1;
               /* Registry branch names are launcher-generated, but treat them as data
                * regardless: a name carrying a quote must never become shell syntax. */
               int other = sess[0] && (!own || !own[0] || strcmp(sess, own) != 0);
               int quotable = !strchr(sess, '\'') && !strchr(sess, '\n');
               if (other && quotable)
               {
                  char cmd[3200];
                  snprintf(cmd, sizeof(cmd),
                           "git -C '%s' rev-parse --verify --quiet '%s^{commit}' >/dev/null 2>&1 "
                           "&& mb=$(git -C '%s' merge-base HEAD '%s' 2>/dev/null) "
                           "&& [ -n \"$mb\" ] "
                           "&& ! git -C '%s' merge-base --is-ancestor \"$mb\" '%s' 2>/dev/null",
                           dir, sess, dir, sess, dir, defref);
                  if (system(cmd) == 0)
                     shared = 1;
               }
            }
            if (!nl)
               break;
            line = nl + 1;
         }
         free(buf);
         return shared;
      }
      char *slash = strrchr(probe, '/');
      if (!slash || slash == probe)
         break;
      *slash = '\0';
   }
   return 0;
}

/* The directory to run git in for `target`, which may name a file (an Edit/Write target)
 * or a directory (a Bash cwd). `git -C <file>` fails, and attn_resolve_default_branch
 * then silently fell back to "main" -- so on a repo whose default is anything else, the
 * refusal named the wrong branch AND compared the base against it. Observed on one repo
 * in one minute: "('testing')" for a Bash op, "('main')" for an Edit. */
void attn_git_dir_for(const char *target, char *out, size_t outlen)
{
   if (!out || !outlen)
      return;
   snprintf(out, outlen, "%s", target ? target : "");
   struct stat st;
   if (out[0] && stat(out, &st) == 0 && S_ISDIR(st.st_mode))
      return;
   /* Walk UP to the nearest EXISTING directory. Stripping a single component is not
    * enough when the target is a new file in a not-yet-created directory: the result
    * is another missing path, so `git -C` cannot run there and BOTH lineage probes
    * (current branch, default ref) fail. The caller reads default_resolved == 0 as an
    * unverifiable lineage and fails closed — so creating a file in a new subdirectory
    * was refused with a branch-lineage error that had nothing to do with the branch,
    * for every session without a registry row (which is every Claude Code session,
    * since EnterWorktree writes none). Failing closed on a genuinely unresolvable
    * default branch is deliberate and is preserved; this only stops a MISSING
    * DIRECTORY from being mistaken for one. */
   char *slash;
   while ((slash = strrchr(out, '/')) != NULL && slash != out)
   {
      *slash = '\0';
      if (stat(out, &st) == 0 && S_ISDIR(st.st_mode))
         return;
   }
}

/* Pure decision for the session-isolation guard (testable in isolation).
 * Returns 1 to BLOCK: a mutating op (SOFT/HARD) whose effective target is NOT
 * inside an aimee-managed worktree. Read / raw-scan ops are never blocked here.
 * The effective target is `file_path` when given (resolved against `cwd` when
 * relative), else `cwd` itself (a Bash mutation runs there). The joined path is
 * lexically normalized before the worktree check so a "../" escape out of the
 * worktree is blocked even though the raw string still contained the marker. */
void attn_session_isolation_target(const char *file_path, const char *cwd, char *out, size_t outsz)
{
   char joined[2048];
   if (file_path && file_path[0] == '/')
      snprintf(joined, sizeof(joined), "%s", file_path);
   else if (file_path && file_path[0])
      snprintf(joined, sizeof(joined), "%s/%s", cwd ? cwd : "", file_path);
   else
      snprintf(joined, sizeof(joined), "%s", cwd ? cwd : "");

   attn_lexical_normalize(joined, out, outsz);
}

int attn_session_isolation_blocked(attn_op_t op, const char *file_path, const char *cwd,
                                   const char *session_id)
{
   if (op != ATTN_OP_SOFT && op != ATTN_OP_HARD)
      return 0;

   char norm[2048];
   attn_session_isolation_target(file_path, cwd, norm, sizeof(norm));
   if (attn_path_in_managed_worktree(norm) || attn_path_is_harness_state(norm) ||
       attn_path_is_session_scratch(norm, session_id))
      return 0;
   return 1;
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

   /* External-memory guard (operator ruling 2026-07-14, default ON via
    * require_aimee_memory): an agent must not author harness-local memory files
    * (~/.claude/projects/<slug>/memory/...) directly — memories go through
    * aimee's memory system. Checked BEFORE the harness-state carve-out in the
    * isolation guard below would allow the write, and inspects Bash command
    * text too, closing the observed cat/sed/python bypass around the file-tool
    * block. */
   if (attn_config_require_aimee_memory())
   {
      char mcwd[1024];
      if (!getcwd(mcwd, sizeof(mcwd)))
         mcwd[0] = '\0';
      const char *mfp = NULL;
      if (ti)
      {
         const char *keys[] = {"file_path", "path", "notebook_path"};
         for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]) && !mfp; k++)
            mfp = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(ti, keys[k]));
      }
      if (attn_external_memory_blocked(op, tool, mfp, bash_cmd, mcwd))
      {
         fprintf(stderr,
                 "aimee attention-guard: refusing a direct write to an external agent-memory "
                 "store (~/.claude/projects/<project>/memory/...). Durable memories must be "
                 "pushed to aimee's memory system, where they are indexed, recalled, and "
                 "audited: use `aimee memory store <key> \"<content>\" [--tier L2] [--kind "
                 "fact]` (recall via `aimee memory search` / the search_memory tool). Reading "
                 "the file store is still allowed. An operator may set require_aimee_memory: "
                 "false in aimee.yaml to disable this — there is no env-var bypass.\n");
         cJSON_Delete(hook);
         free(stdin_data);
         return 2;
      }
   }

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
      /* Branch lineage: being INSIDE a managed worktree is not enough. The worktree
       * must have been created by the launcher (so a registry row exists) and rooted
       * correctly -- a primary on the default branch, a delegate on its parent's.
       * Checked before the path test so a correctly-located but wrongly-rooted
       * worktree is still refused. */
      char lin_target[2048];
      attn_session_isolation_target(fp, cwd, lin_target, sizeof(lin_target));
      if (attn_path_in_managed_worktree(lin_target))
      {
         char base[256], own[256], defbr[256];
         /* git needs a DIRECTORY: lin_target is the file for an Edit/Write. */
         char gitdir[2048];
         attn_git_dir_for(lin_target, gitdir, sizeof(gitdir));
         int reg_state = attn_registry_base_for(lin_target, base, sizeof(base), own, sizeof(own));
         attn_resolve_default_branch(gitdir, defbr, sizeof(defbr));
         int parent_registered = (base[0] && own[0] && strcmp(base, own) != 0)
                                     ? attn_registry_branch_is_registered(lin_target, base)
                                     : 0;
         /* reg_state: 2 row found, 1 registry exists but no row, 0 no registry at all.
          *   0 -> the repo has never used managed worktrees; there is nothing to check
          *        against, and blocking would break every non-aimee checkout.
          *   2 with an EMPTY base -> a row written by an older launcher, before the base
          *        column existed. Grandfathered: the launcher did create it, and blocking
          *        would break every in-flight session the moment this ships.
          *   1 -> a managed repo where this worktree was never registered. It is NOT
          *        necessarily hand-rolled: only workspace.c writes rows, and Claude
          *        Code's EnterWorktree (named as sanctioned in the refusal below) writes
          *        none. Decide those on git topology instead -- see
          *        attn_unregistered_lineage_blocked. */
         if (reg_state == 1)
         {
            char defref[300], gitown[256];
            attn_git_current_branch(gitdir, gitown, sizeof(gitown));
            int resolved = attn_git_default_ref(gitdir, defbr, defref, sizeof(defref));
            int foreign =
                resolved && attn_git_shares_foreign_session_history(gitdir, defref, gitown);
            if (attn_unregistered_lineage_blocked(resolved, foreign))
            {
               fprintf(stderr,
                       "aimee attention-guard: refusing a mutating op — the worktree at '%s' is on "
                       "branch '%s', which has no launcher registry row and %s. A primary session "
                       "must branch off the default branch ('%s'); a delegate off its parent. "
                       "Recreate the session worktree with the launcher (Claude Code: "
                       "EnterWorktree; aimee: launch `aimee`) instead of `git worktree add` by "
                       "hand.\n",
                       lin_target, gitown[0] ? gitown : "(unknown)",
                       resolved ? "carries commits from another session's branch"
                                : "could not be checked, because the default branch does not "
                                  "resolve here",
                       defbr[0] ? defbr : "(unresolved)");
               cJSON_Delete(hook);
               free(stdin_data);
               return 2;
            }
         }
         else if (reg_state == 2 && base[0] &&
                  attn_session_branch_blocked(base, defbr, parent_registered))
         {
            fprintf(stderr,
                    "aimee attention-guard: refusing a mutating op — the worktree at '%s' is on "
                    "branch '%s' rooted at '%s', which is neither the default branch ('%s') nor a "
                    "registered parent session branch. A primary session must branch off the "
                    "default branch; a delegate off its parent. Recreate the session worktree "
                    "with the launcher (Claude Code: EnterWorktree; aimee: launch `aimee`) instead "
                    "of `git worktree add` by hand.\n",
                    lin_target, own[0] ? own : "(unknown)", base,
                    defbr[0] ? defbr : "(unresolved)");
            cJSON_Delete(hook);
            free(stdin_data);
            return 2;
         }
      }

      /* Bash reaching out of the worktree: judged on the command text, because the
       * isolation check below only sees the cwd for a Bash call. */
      if (bash_cmd && bash_cmd[0] && attn_bash_escapes_worktree(bash_cmd, cwd))
      {
         fprintf(stderr,
                 "aimee attention-guard: refusing a Bash command that writes outside this "
                 "session's worktree. The command changes directory to, or redirects into, an "
                 "absolute path that is not under .aimee/worktrees/ or .claude/worktrees/. The "
                 "cwd being a valid worktree is not enough — what the command TOUCHES has to "
                 "stay inside it. Do the work in the session worktree, or ask the operator to "
                 "make the change.\n");
         cJSON_Delete(hook);
         free(stdin_data);
         return 2;
      }

      if (attn_session_isolation_blocked(op, fp, cwd, sid))
      {
         /* Name the path that was actually judged. When an absolute file_path is
          * given it IS the effective target, and the cwd may be a perfectly good
          * managed worktree — saying otherwise sends the reader off diagnosing a
          * worktree that is not the problem. */
         char target[2048];
         attn_session_isolation_target(fp, cwd, target, sizeof(target));
         char where[2300];
         if (fp && fp[0])
            snprintf(where, sizeof(where), "the write target '%s' is not inside a managed worktree",
                     target);
         else
            snprintf(where, sizeof(where),
                     "this session is operating in '%s', which is not a "
                     "managed worktree",
                     cwd[0] ? cwd : "(unknown cwd)");
         fprintf(stderr,
                 "aimee attention-guard: refusing a mutating op outside a session worktree — %s "
                 "(.aimee/worktrees/... or .claude/worktrees/...). aimee requires each mutating "
                 "session to run in an isolated worktree+branch off the default branch: create "
                 "one (Claude Code: EnterWorktree; aimee: launch `aimee`, which materializes a "
                 "worktree and chdirs into it). An operator may set require_session_worktree: "
                 "false in aimee.yaml to disable this — there is no env-var bypass.\n",
                 where);
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
                    "indexes this repo — explore through it instead: " AIMEE_CODE_TOOL_FIND_SYMBOL
                    ", " AIMEE_CODE_TOOL_AST_GREP_SEARCH ", " AIMEE_CODE_TOOL_INDEX
                    " command=" AIMEE_CODE_INDEX_COMMAND_HYBRID ", or get_context_block. Raise "
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
