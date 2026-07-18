/* guardrails.c: path classification and session policy state. Pre-tool
 * enforcement (pre_tool_check) lives in guardrails_orchestrator.c; TDD
 * state in guardrails_tdd.c. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aimee.h"
#include "aimee_home.h"
#include "cJSON.h"
#include "guardrails_internal.h"
#include "guardrails_blast_radius.h"
#include "git_verify.h"
#include "kb_client.h"
#include "log.h"
#include "platform_path.h"
#include "slop_detect.h"
#include <ctype.h>
#include <dirent.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

/* Scan C/H source content for bare (unescaped) newlines inside string literals.
 * Returns 1 if found, 0 if clean.
 *
 * A literal newline in a string literal is a C syntax error; delegates sometimes
 * write printf("ok\n") where the \n is a real newline rather than an escape
 * sequence. This scanner detects that so the guardrail can block the write.
 *
 * The scanner handles:
 *   - line comments (//)
 *   - block comments
 *   - string literals ("...")
 *   - character literals ('...')  -- important: prevents '"' from triggering string mode
 */
int c_source_has_bare_string_newline(const char *content)
{
   int in_string = 0;
   int in_char = 0;
   int in_line_comment = 0;
   int in_block_comment = 0;
   const char *p;

   if (!content)
      return 0;

   for (p = content; *p; p++)
   {
      if (in_block_comment)
      {
         if (p[0] == '*' && p[1] == '/')
         {
            in_block_comment = 0;
            p++; /* skip '/' */
         }
         continue;
      }
      if (in_line_comment)
      {
         if (*p == '\n')
            in_line_comment = 0;
         continue;
      }
      if (in_char)
      {
         if (*p == '\\')
         {
            p++; /* skip escaped char inside char literal */
            continue;
         }
         if (*p == '\'')
            in_char = 0;
         continue; /* don't treat '"' inside a char literal as opening a string */
      }
      if (in_string)
      {
         if (*p == '\\')
         {
            p++; /* skip escaped char — handles \n, \", \\, and backslash-newline continuations */
            continue;
         }
         if (*p == '"')
         {
            in_string = 0;
            continue;
         }
         if (*p == '\n')
            return 1; /* bare newline inside string literal */
         continue;
      }
      /* Outside string / comment */
      if (p[0] == '/' && p[1] == '/')
      {
         in_line_comment = 1;
         p++;
         continue;
      }
      if (p[0] == '/' && p[1] == '*')
      {
         in_block_comment = 1;
         p++;
         continue;
      }
      if (*p == '\'')
      {
         in_char = 1;
         continue;
      }
      if (*p == '"')
      {
         in_string = 1;
         continue;
      }
   }
   return 0;
}

/* Anti-pattern in-session hit tracking lives on session_state_t so it
 * (a) resets naturally across sessions, (b) survives server restarts without
 * becoming permanently stuck, and (c) can be cleared by
 * `aimee memory antipattern reset`. The threshold is defined in guardrails.h. */

#define POLICY_MAX_ITEMS 64

typedef struct
{
   char *items[POLICY_MAX_ITEMS];
   int count;
} policy_list_t;

typedef struct
{
   policy_list_t sensitive_patterns;
   policy_list_t sensitive_exact;
   policy_list_t db_extensions;
   policy_list_t path_denies;
   policy_list_t write_cmds;
   policy_list_t git_write_cmds;
   policy_list_t package_cmds;
   char loaded_path[MAX_PATH_LEN];
   time_t loaded_mtime;
   int loaded_exists;
   int valid;
} guardrail_policy_t;

static pthread_mutex_t g_policy_mu = PTHREAD_MUTEX_INITIALIZER;
static guardrail_policy_t g_policy;

static const char *default_sensitive_patterns[] = {".env",     ".env.", "credentials", "secrets",
                                                   "password", ".key",  ".pem",        ".crt",
                                                   ".p12",     ".pfx",  NULL};
static const char *default_sensitive_exact[] = {
    ".env",         ".env.local", ".env.production", "credentials.json",
    "secrets.json", "id_rsa",     "id_ed25519",      NULL};
static const char *default_db_extensions[] = {".db", ".sqlite", ".sqlite3", NULL};
static const char *default_path_denies[] = {
    ".ssh/", ".gnupg/", ".aws/credentials", ".env", "/etc/shadow", "/etc/passwd", NULL};
static const char *default_write_cmds[] = {"rm ",    "rm\t",   "rmdir ",    "mv ",      "cp ",
                                           "chmod ", "chown ", "mkdir ",    "touch ",   "tee ",
                                           "dd ",    "ln ",    "truncate ", "install ", NULL};
static const char *default_git_write_cmds[] = {
    "git commit",  "git push",      "git pull",      "git fetch", "git clone",
    "git reset",   "git checkout",  "git rebase",    "git merge", "git stash",
    "git clean",   "git branch -d", "git branch -D", "git tag",   "git add",
    "git restore", "git rm",        "git mv",        NULL};
static const char *default_package_cmds[] = {
    "pip install", "pip uninstall", "npm install", "npm uninstall", "apt-get install",
    "apt install", "cargo install", "go install",  "dnf install",   "dnf remove",
    "dnf erase",   "dnf upgrade",   "dnf update",  "yum install",   "yum remove",
    "yum erase",   "yum upgrade",   "yum update",  "rpm -i",        "rpm -e",
    "rpm -U",      "rpm --install", "rpm --erase", "rpm --upgrade", NULL};

static void policy_list_clear(policy_list_t *list)
{
   if (!list)
      return;
   for (int i = 0; i < list->count; i++)
      free(list->items[i]);
   memset(list, 0, sizeof(*list));
}

static void policy_reset_locked(void)
{
   policy_list_clear(&g_policy.sensitive_patterns);
   policy_list_clear(&g_policy.sensitive_exact);
   policy_list_clear(&g_policy.db_extensions);
   policy_list_clear(&g_policy.path_denies);
   policy_list_clear(&g_policy.write_cmds);
   policy_list_clear(&g_policy.git_write_cmds);
   policy_list_clear(&g_policy.package_cmds);
   memset(g_policy.loaded_path, 0, sizeof(g_policy.loaded_path));
   g_policy.loaded_mtime = 0;
   g_policy.loaded_exists = 0;
   g_policy.valid = 0;
}

static void policy_list_add_literals(policy_list_t *list, const char *const *items)
{
   if (!list || !items)
      return;
   for (int i = 0; items[i] && list->count < POLICY_MAX_ITEMS; i++)
   {
      list->items[list->count] = strdup(items[i]);
      if (list->items[list->count])
         list->count++;
   }
}

static void policy_load_defaults_locked(void)
{
   policy_list_add_literals(&g_policy.sensitive_patterns, default_sensitive_patterns);
   policy_list_add_literals(&g_policy.sensitive_exact, default_sensitive_exact);
   policy_list_add_literals(&g_policy.db_extensions, default_db_extensions);
   policy_list_add_literals(&g_policy.path_denies, default_path_denies);
   policy_list_add_literals(&g_policy.write_cmds, default_write_cmds);
   policy_list_add_literals(&g_policy.git_write_cmds, default_git_write_cmds);
   policy_list_add_literals(&g_policy.package_cmds, default_package_cmds);
}

static char *slurp_text_file(const char *path)
{
   FILE *fp;
   long len;
   char *buf;

   if (!path)
      return NULL;
   fp = fopen(path, "rb");
   if (!fp)
      return NULL;
   if (fseek(fp, 0, SEEK_END) != 0)
   {
      fclose(fp);
      return NULL;
   }
   len = ftell(fp);
   if (len < 0 || fseek(fp, 0, SEEK_SET) != 0)
   {
      fclose(fp);
      return NULL;
   }
   buf = calloc((size_t)len + 1, 1);
   if (!buf)
   {
      fclose(fp);
      return NULL;
   }
   if (len > 0 && fread(buf, 1, (size_t)len, fp) != (size_t)len)
   {
      free(buf);
      fclose(fp);
      return NULL;
   }
   fclose(fp);
   return buf;
}

static void policy_load_json_list(policy_list_t *list, cJSON *root, const char *key)
{
   cJSON *arr;
   if (!list || !root || !key)
      return;
   arr = cJSON_GetObjectItemCaseSensitive(root, key);
   if (!cJSON_IsArray(arr))
      return;
   policy_list_clear(list);
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, arr)
   {
      if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0])
         continue;
      if (list->count >= POLICY_MAX_ITEMS)
         break;
      list->items[list->count] = strdup(item->valuestring);
      if (list->items[list->count])
         list->count++;
   }
}

static void guardrails_policy_path(char *buf, size_t len)
{
   const char *override = getenv("AIMEE_GUARDRAILS_PATH");
   const char *config_dir = aimee_home();

   if (!buf || len == 0)
      return;
   if (override && override[0])
   {
      snprintf(buf, len, "%s", override);
      return;
   }
   if (config_dir && config_dir[0])
   {
      snprintf(buf, len, "%s/guardrails.json", config_dir);
      return;
   }
   buf[0] = '\0';
}

static void guardrails_policy_ensure_loaded(void)
{
   char path[MAX_PATH_LEN];
   struct stat st;
   int exists;
   time_t mtime = 0;

   guardrails_policy_path(path, sizeof(path));
   exists = (path[0] && stat(path, &st) == 0);
   if (exists)
      mtime = st.st_mtime;

   pthread_mutex_lock(&g_policy_mu);
   if (g_policy.valid && strcmp(g_policy.loaded_path, path) == 0 &&
       g_policy.loaded_exists == exists && (!exists || g_policy.loaded_mtime == mtime))
   {
      pthread_mutex_unlock(&g_policy_mu);
      return;
   }

   policy_reset_locked();
   policy_load_defaults_locked();
   if (exists)
   {
      char *raw = slurp_text_file(path);
      if (raw)
      {
         cJSON *root = cJSON_Parse(raw);
         if (cJSON_IsObject(root))
         {
            policy_load_json_list(&g_policy.sensitive_patterns, root, "sensitive_patterns");
            policy_load_json_list(&g_policy.sensitive_exact, root, "sensitive_exact");
            policy_load_json_list(&g_policy.db_extensions, root, "db_extensions");
            policy_load_json_list(&g_policy.path_denies, root, "path_denies");
            policy_load_json_list(&g_policy.write_cmds, root, "write_commands");
            policy_load_json_list(&g_policy.git_write_cmds, root, "git_write_commands");
            policy_load_json_list(&g_policy.package_cmds, root, "package_commands");
         }
         cJSON_Delete(root);
         free(raw);
      }
   }
   snprintf(g_policy.loaded_path, sizeof(g_policy.loaded_path), "%s", path);
   g_policy.loaded_exists = exists;
   g_policy.loaded_mtime = mtime;
   g_policy.valid = 1;
   pthread_mutex_unlock(&g_policy_mu);
}

void guardrails_policy_reset(void)
{
   pthread_mutex_lock(&g_policy_mu);
   policy_reset_locked();
   pthread_mutex_unlock(&g_policy_mu);
}

static int policy_list_matches_exact(const policy_list_t *list, const char *value)
{
   if (!list || !value)
      return 0;
   for (int i = 0; i < list->count; i++)
   {
      if (list->items[i] && strcmp(value, list->items[i]) == 0)
         return 1;
   }
   return 0;
}

static int policy_list_matches_substring(const policy_list_t *list, const char *value)
{
   if (!list || !value)
      return 0;
   for (int i = 0; i < list->count; i++)
   {
      if (list->items[i] && strstr(value, list->items[i]) != NULL)
         return 1;
   }
   return 0;
}

static const char *policy_list_matching_substring(const policy_list_t *list, const char *value)
{
   if (!list || !value)
      return NULL;
   for (int i = 0; i < list->count; i++)
   {
      if (list->items[i] && strstr(value, list->items[i]) != NULL)
         return list->items[i];
   }
   return NULL;
}

static int policy_list_matches_prefix(const policy_list_t *list, const char *value)
{
   if (!list || !value)
      return 0;
   for (int i = 0; i < list->count; i++)
   {
      size_t n;
      if (!list->items[i])
         continue;
      n = strlen(list->items[i]);
      if (strncmp(value, list->items[i], n) == 0)
         return 1;
   }
   return 0;
}

const char *basename_of(const char *path)
{
   const char *slash = strrchr(path, '/');
   return slash ? slash + 1 : path;
}

static int is_sensitive_exact(const char *filename)
{
   guardrails_policy_ensure_loaded();
   return policy_list_matches_exact(&g_policy.sensitive_exact, filename);
}

static int has_db_extension(const char *filename)
{
   const char *dot = strrchr(filename, '.');
   if (!dot)
      return 0;
   guardrails_policy_ensure_loaded();
   return policy_list_matches_exact(&g_policy.db_extensions, dot);
}

int is_sensitive_file(const char *path)
{
   if (!path)
      return 0;
   guardrails_policy_ensure_loaded();
   return policy_list_matches_substring(&g_policy.sensitive_patterns, path);
}

classification_t classify_path(const char *file_path)
{
   classification_t result;
   memset(&result, 0, sizeof(result));
   snprintf(result.path, sizeof(result.path), "%s", file_path);
   result.severity = SEV_GREEN;

   const char *fname = basename_of(file_path);

   /* Check exact matches first */
   if (is_sensitive_exact(fname))
   {
      result.severity = SEV_BLOCK;
      snprintf(result.reason, sizeof(result.reason), "sensitive file");
      return result;
   }

   /* Check sensitive patterns */
   guardrails_policy_ensure_loaded();
   {
      const char *pattern = policy_list_matching_substring(&g_policy.sensitive_patterns, fname);
      if (pattern)
      {
         result.severity = SEV_BLOCK;
         snprintf(result.reason, sizeof(result.reason), "sensitive file pattern: %s", pattern);
         return result;
      }
   }

   /* Check database extensions */
   if (has_db_extension(fname))
   {
      result.severity = SEV_RED;
      snprintf(result.reason, sizeof(result.reason), "database file");
      return result;
   }

   /* Check blast radius via the structural code index (shared resolver — also
    * used by the §7 blast-radius advisory; see guardrails_blast_radius.c). */
   {
      blast_radius_t br;
      if (guardrails_blast_radius_for_abs_path(file_path, &br) == 0)
      {
         if (br.dependent_count >= 5)
         {
            result.severity = SEV_RED;
            snprintf(result.reason, sizeof(result.reason), "High blast radius: %d dependents",
                     br.dependent_count);
            return result;
         }
         if (br.dependent_count >= 1)
         {
            result.severity = SEV_YELLOW;
            snprintf(result.reason, sizeof(result.reason), "Moderate blast radius: %d dependents",
                     br.dependent_count);
            return result;
         }
      }
   }

   return result;
}

char *normalize_path(const char *path, const char *cwd, char *buf, size_t buf_len)
{
   if (!path || !*path)
   {
      buf[0] = '\0';
      return buf;
   }

   /* ~/... paths: expand to $HOME */
   if (path[0] == '~' && (path[1] == '/' || path[1] == '\0'))
   {
      const char *home = platform_home_dir();
      if (home)
         snprintf(buf, buf_len, "%s%s", home, path + 1);
      else
         snprintf(buf, buf_len, "%s", path);
   }
   else if (path[0] == '/')
   {
      snprintf(buf, buf_len, "%s", path);
   }
   else if (cwd && *cwd)
   {
      snprintf(buf, buf_len, "%s/%s", cwd, path);
   }
   else
   {
      snprintf(buf, buf_len, "%s", path);
   }

   /* Canonicalize: resolve symlinks and .. components.
    * If the path exists, realpath resolves everything.
    * If not, try resolving the parent and appending the basename.
    * If neither resolves, keep the lexical path (safe fallback for
    * paths on nonexistent filesystems or in tests). */
   char resolved[MAX_PATH_LEN];
   if (realpath(buf, resolved))
   {
      snprintf(buf, buf_len, "%s", resolved);
      return buf;
   }

   char tmp[MAX_PATH_LEN];
   snprintf(tmp, sizeof(tmp), "%s", buf);
   char *slash = strrchr(tmp, '/');
   if (slash && slash != tmp)
   {
      *slash = '\0';
      if (realpath(tmp, resolved))
         snprintf(buf, buf_len, "%s/%s", resolved, slash + 1);
   }

   /* Lexical collapse of . and .. components for paths that could not be resolved.
    * This prevents traversal-like constructs from bypassing prefix checks. */
   if (strstr(buf, "/./") || strstr(buf, "/../"))
   {
      char parts[MAX_PATH_LEN];
      snprintf(parts, sizeof(parts), "%s", buf);
      char *stack[256];
      int depth = 0;
      char *saveptr = NULL;
      char *seg = strtok_r(parts, "/", &saveptr);
      while (seg)
      {
         if (strcmp(seg, ".") == 0)
         {
            /* skip */
         }
         else if (strcmp(seg, "..") == 0)
         {
            if (depth > 0)
               depth--;
         }
         else
         {
            if (depth < 256)
               stack[depth++] = seg;
            /* else: silently drop excess segments to prevent overflow */
         }
         seg = strtok_r(NULL, "/", &saveptr);
      }
      size_t pos = 0;
      for (int i = 0; i < depth && pos < buf_len - 2; i++)
      {
         buf[pos++] = '/';
         size_t slen = strlen(stack[i]);
         if (pos + slen >= buf_len)
            break;
         memcpy(buf + pos, stack[i], slen);
         pos += slen;
      }
      if (pos == 0 && buf_len > 1)
         buf[pos++] = '/';
      buf[pos] = '\0';
   }

   return buf;
}

/* Shared filesystem path validation for all agent tool paths.
 * Resolves symlinks via realpath, rejects traversal, rejects sensitive paths.
 * Returns NULL on success, or a static error string on failure. */
const char *guardrails_validate_file_path(const char *path, char *resolved_buf, size_t resolved_len)
{
   guardrails_policy_ensure_loaded();

   if (!path || !path[0])
      return "error: empty path";
   if (strstr(path, "/../") || strstr(path, "/..") == path + strlen(path) - 3)
      return "error: path traversal not allowed";
   /* Also reject leading ../ */
   if (strncmp(path, "../", 3) == 0 || strcmp(path, "..") == 0)
      return "error: path traversal not allowed";

   if (realpath(path, resolved_buf) == NULL)
   {
      /* For write, file may not exist yet -- resolve parent */
      char parent[MAX_PATH_LEN];
      snprintf(parent, sizeof(parent), "%s", path);
      char *last_slash = strrchr(parent, '/');
      if (last_slash)
      {
         *last_slash = '\0';
         if (realpath(parent, resolved_buf) == NULL)
            return NULL; /* Let fopen handle the error */
      }
      else
         return NULL;
   }

   /* Check resolved path against sensitive deny list */
   for (int i = 0; i < g_policy.path_denies.count; i++)
   {
      if (g_policy.path_denies.items[i] && strstr(resolved_buf, g_policy.path_denies.items[i]))
         return "error: access to sensitive path denied";
   }

   /* Check for symlink escape: resolved path should not point into a
    * sensitive directory even if the original path looked benign.  Compare
    * against the same deny list (already done above on the resolved path). */

   return NULL;
}

/* Copy `in` to `out`, replacing characters inside single- or double-quoted
 * regions (and backslash-escaped chars) with spaces. Quote delimiters
 * themselves are preserved. Follows POSIX shell conventions: single quotes
 * are literal; double quotes honor '\' escapes. This lets downstream scans
 * for redirection/separator tokens ignore characters like '>' that appear
 * inside quoted arguments (e.g. `grep "stores->shared" file`). */
static void mask_quoted_regions(const char *in, char *out, size_t outcap)
{
   if (outcap == 0)
      return;
   size_t oi = 0;
   char quote = 0;
   for (const char *p = in; *p && oi + 1 < outcap; p++)
   {
      char c = *p;
      if (quote == '\'')
      {
         if (c == '\'')
         {
            quote = 0;
            out[oi++] = c;
         }
         else
            out[oi++] = ' ';
         continue;
      }
      if (quote == '"')
      {
         if (c == '\\' && p[1])
         {
            out[oi++] = ' ';
            if (oi + 1 >= outcap)
               break;
            out[oi++] = ' ';
            p++;
            continue;
         }
         if (c == '"')
         {
            quote = 0;
            out[oi++] = c;
         }
         else
            out[oi++] = ' ';
         continue;
      }
      if (c == '\\' && p[1])
      {
         out[oi++] = ' ';
         if (oi + 1 >= outcap)
            break;
         out[oi++] = ' ';
         p++;
         continue;
      }
      if (c == '\'' || c == '"')
      {
         quote = c;
         out[oi++] = c;
         continue;
      }
      out[oi++] = c;
   }
   out[oi < outcap ? oi : outcap - 1] = '\0';
}

static int redirection_target_is_dev_null(const char *p)
{
   while (*p == '>' || *p == '<')
      p++;
   while (*p && isspace((unsigned char)*p))
      p++;

   if (strncmp(p, "/dev/null", 9) != 0)
      return 0;

   p += 9;
   return *p == '\0' || isspace((unsigned char)*p) || *p == '|' || *p == '&' || *p == ';';
}

int is_write_command(const char *command)
{
   if (!command)
      return 0;

   guardrails_policy_ensure_loaded();

   /* Skip leading whitespace */
   while (*command && isspace((unsigned char)*command))
      command++;

   if (policy_list_matches_prefix(&g_policy.write_cmds, command) ||
       policy_list_matches_prefix(&g_policy.git_write_cmds, command) ||
       policy_list_matches_prefix(&g_policy.package_cmds, command))
      return 1;

   /* Scan on a copy with quoted-region contents masked to spaces so a '>'
    * inside a grep pattern or similar doesn't look like a redirection. */
   size_t clen = strlen(command);
   char *scan_heap = NULL;
   char stack_buf[1024];
   char *scan;
   if (clen + 1 <= sizeof(stack_buf))
      scan = stack_buf;
   else
   {
      scan_heap = (char *)malloc(clen + 1);
      if (!scan_heap)
         return 0;
      scan = scan_heap;
   }
   mask_quoted_regions(command, scan, clen + 1);

   int result = 0;

   /* Detect redirections with or without leading space: e.g. "echo hi>file".
    * Skip fd-to-fd redirections (N>&M) and noise-suppression redirects to
    * /dev/null, which should not make read-only discovery commands count as writes. */
   for (const char *c = scan; *c; c++)
   {
      if (c == scan)
         continue;
      if (*c == '>' && c[1] == '&')
         continue; /* fd-to-fd: >&N */
      if ((*c == '1' || *c == '2') && c[1] == '>' && c[2] == '&')
         continue; /* fd-to-fd: N>&M */
      if (*c == '>' && redirection_target_is_dev_null(c))
         continue;
      if ((*c == '1' || *c == '2') && c[1] == '>' && redirection_target_is_dev_null(c + 1))
         continue;
      if (*c == '>' || ((*c == '1' || *c == '2') && c[1] == '>'))
      {
         result = 1;
         goto done;
      }
   }

   /* Check for in-place editing flags */
   if (strstr(scan, "sed -i") || strstr(scan, "perl -pi"))
   {
      result = 1;
      goto done;
   }

   /* Check for write commands after pipe or semicolons in compound commands */
   {
      const char *separators[] = {" | ", " || ", " && ", "; ", NULL};
      for (int s = 0; separators[s]; s++)
      {
         const char *pos = scan;
         while ((pos = strstr(pos, separators[s])) != NULL)
         {
            pos += strlen(separators[s]);
            /* Skip whitespace after separator */
            while (*pos == ' ' || *pos == '\t')
               pos++;
            /* Check if the sub-command after the separator is a write command */
            if (policy_list_matches_prefix(&g_policy.write_cmds, pos) ||
                policy_list_matches_prefix(&g_policy.git_write_cmds, pos) ||
                policy_list_matches_prefix(&g_policy.package_cmds, pos))
            {
               result = 1;
               goto done;
            }
         }
      }
   }

done:
   free(scan_heap);
   return result;
}
