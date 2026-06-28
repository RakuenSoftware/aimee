/* canonical_index.c: project/org-level code index, owned by aimee-kb.
 *
 * Storage: DB2 (Postgres). All paths use aimee_pg_* (libpq) directly.
 * Tables: projects, files, terms, file_imports, file_exports,
 * code_calls, file_contents (declared in db2/schema.sql, applied
 * by db2_init).
 *
 * Scan coordination is the caller's responsibility (kb_service holds a
 * single-slot lock with a 60-second cooldown). This module assumes
 * exactly one writer at a time.
 */

#include "canonical_index.h"
#include "code_index.h"
#include "cross_repo_resolver.h" /* H0b: xrepo_lang_name / xrepo_path_is_vendored */
#include "config.h"
#include "css_graph.h" /* CSS migration assistant: style graph + component join (WP-C/D) */
#include "db2.h"
#include "db2_internal.h"

#include "aimee.h"
#include "db_postgres.h"
#include "log.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define CI_LOG_TAG  "db2.canonical_index"
#define CI_ERRBUF   256
#define CI_MAX_DEPS 64
#define CI_MAX_DEFS 256

static const char *ci_get_extension(const char *path)
{
   const char *dot = strrchr(path, '.');
   if (!dot || dot == path)
      return "";
   return dot;
}

/* ---- Connection access ----------------------------------------- */

static void *ci_conn(void)
{
   return db2_conn();
}

/* ---- Project / file resolution --------------------------------- */

static int64_t ci_resolve_project_id(void *conn, const char *name)
{
   char err[CI_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT id FROM projects WHERE name = ?1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", name);
   int64_t id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

static int64_t ci_resolve_file_id(void *conn, int64_t project_id, const char *rel_path)
{
   char err[CI_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT id FROM files WHERE project_id = ?1 AND path = ?2", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", project_id);
   aimee_pg_bind_text(st, "?2", rel_path);
   int64_t id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

static int64_t ci_upsert_project(void *conn, const char *name, const char *root)
{
   char ts[32];
   now_utc(ts, sizeof(ts));
   char err[CI_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO projects (name, root, scanned_at) VALUES (?1, ?2, ?3) "
                        "ON CONFLICT (name) DO UPDATE SET root = EXCLUDED.root,"
                        " scanned_at = EXCLUDED.scanned_at "
                        "RETURNING id",
                        err, sizeof(err));
   if (!st)
   {
      LOG_ERROR(CI_LOG_TAG, "upsert_project prepare failed: %s", err);
      return -1;
   }
   aimee_pg_bind_text(st, "?1", name);
   aimee_pg_bind_text(st, "?2", root);
   aimee_pg_bind_text(st, "?3", ts);
   int64_t id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

/* FNV-1a 64-bit hex of file content. Stored on files.hash so the code-embed
 * pass can skip unchanged files (content-hash idempotency: a re-scan with no
 * edits performs zero embedder calls). */
static void ci_content_hash(const char *content, char *out, size_t out_len)
{
   unsigned long long h = 1469598103934665603ULL;
   for (const unsigned char *p = (const unsigned char *)(content ? content : ""); *p; p++)
   {
      h ^= (unsigned long long)*p;
      h *= 1099511628211ULL;
   }
   snprintf(out, out_len, "%016llx", h);
}

static int64_t ci_upsert_file(void *conn, int64_t project_id, const char *rel_path,
                              const char *scanned_at)
{
   char err[CI_ERRBUF] = "";
   /* H0b: per-file language + vendored flag, derived from the path at index time. */
   const char *language = xrepo_lang_name(xrepo_lang_from_path(rel_path));
   int vendored = xrepo_path_is_vendored(rel_path);
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "INSERT INTO files (project_id, path, scanned_at, language, vendored) "
       "VALUES (?1, ?2, ?3, ?4, ?5) "
       "ON CONFLICT (project_id, path) DO UPDATE SET scanned_at = EXCLUDED.scanned_at, "
       "language = EXCLUDED.language, vendored = EXCLUDED.vendored "
       "RETURNING id",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", project_id);
   aimee_pg_bind_text(st, "?2", rel_path);
   aimee_pg_bind_text(st, "?3", scanned_at);
   aimee_pg_bind_text(st, "?4", language);
   aimee_pg_bind_int(st, "?5", vendored);
   int64_t id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

/* Returns 1 if the file's mtime is newer than its stored scanned_at,
 * 0 if up-to-date, 1 if no row exists (treat as modified). */
static int ci_file_modified_since(void *conn, int64_t project_id, const char *rel_path,
                                  time_t mtime)
{
   char err[CI_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT scanned_at FROM files WHERE project_id = ?1 AND path = ?2", err, sizeof(err));
   if (!st)
      return 1;
   aimee_pg_bind_int64(st, "?1", project_id);
   aimee_pg_bind_text(st, "?2", rel_path);
   int modified = 1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *ts = aimee_pg_column_text(st, 0);
      if (ts && ts[0])
      {
         struct tm tm;
         memset(&tm, 0, sizeof(tm));
         /* Accept either ISO ("2024-01-01T12:00:00") or space-separated form. */
         char sep = '\0';
         int matched = sscanf(ts, "%d-%d-%d%c%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &sep,
                              &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
         if (matched == 7)
         {
            tm.tm_year -= 1900;
            tm.tm_mon -= 1;
            /* now_utc() writes UTC; parse as UTC (timegm), not local (mktime),
             * otherwise files modified within the local timezone offset of
             * the previous scan are wrongly skipped. */
            time_t scanned = timegm(&tm);
            if (mtime <= scanned)
               modified = 0;
         }
      }
   }
   aimee_pg_finalize(st);
   return modified;
}

/* ---- File body replacement (transactional) --------------------- */

static void ci_replace_file_data(void *conn, int64_t file_id, const char *ext, const char *content)
{
   char err[CI_ERRBUF] = "";

   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
   {
      LOG_WARN(CI_LOG_TAG, "BEGIN failed: %s", err);
      return;
   }

   db2_exec_conn_int64(conn, "DELETE FROM file_exports WHERE file_id = ?1", file_id);
   db2_exec_conn_int64(conn, "DELETE FROM file_imports WHERE file_id = ?1", file_id);
   db2_exec_conn_int64(conn, "DELETE FROM terms WHERE file_id = ?1", file_id);
   db2_exec_conn_int64(conn, "DELETE FROM code_calls WHERE file_id = ?1", file_id);

   /* Record the content hash so the code-embed pass can skip unchanged files. */
   {
      char chash[32];
      ci_content_hash(content, chash, sizeof(chash));
      aimee_pg_stmt_t *hst =
          aimee_pg_prepare(conn, "UPDATE files SET hash = ?1 WHERE id = ?2", err, sizeof(err));
      if (hst)
      {
         aimee_pg_bind_text(hst, "?1", chash);
         aimee_pg_bind_int64(hst, "?2", file_id);
         (void)aimee_pg_step(hst, err, sizeof(err));
         aimee_pg_finalize(hst);
      }
   }

   {
      aimee_pg_stmt_t *st =
          aimee_pg_prepare(conn,
                           "INSERT INTO file_contents (file_id, content) VALUES (?1, ?2) "
                           "ON CONFLICT (file_id) DO UPDATE SET content = EXCLUDED.content",
                           err, sizeof(err));
      if (st)
      {
         aimee_pg_bind_int64(st, "?1", file_id);
         aimee_pg_bind_text(st, "?2", content);
         aimee_pg_step(st, err, sizeof(err));
         aimee_pg_finalize(st);
      }
   }

   /* Exports. */
   {
      char *exports[128];
      int n = extract_exports(ext, content, exports, 128);
      aimee_pg_stmt_t *st = aimee_pg_prepare(
          conn, "INSERT INTO file_exports (file_id, name) VALUES (?1, ?2)", err, sizeof(err));
      for (int i = 0; i < n; i++)
      {
         if (st)
         {
            aimee_pg_bind_int64(st, "?1", file_id);
            aimee_pg_bind_text(st, "?2", exports[i]);
            aimee_pg_step(st, err, sizeof(err));
            aimee_pg_reset(st);
         }
         free(exports[i]);
      }
      if (st)
         aimee_pg_finalize(st);
   }

   /* Imports. H6: also persist is_system (C/C++ angle `#include <...>`) so the
    * cross-repo route builder can apply prefer-local to quoted includes only. */
   {
      /* 256 (was 128): H6 also captures angle <lib.h> includes, so include-heavy
       * C/C++ files have more imports (system headers are skipped at extraction,
       * but real ones can still be many) — headroom so a real dep is not truncated. */
      char *imports[256];
      int imp_sys[256] = {0}; /* defensive: extract_imports_sys also zeroes it */
      int n = extract_imports_sys(ext, content, imports, imp_sys, 256);
      aimee_pg_stmt_t *st = aimee_pg_prepare(
          conn, "INSERT INTO file_imports (file_id, name, is_system) VALUES (?1, ?2, ?3)", err,
          sizeof(err));
      for (int i = 0; i < n; i++)
      {
         if (st)
         {
            aimee_pg_bind_int64(st, "?1", file_id);
            aimee_pg_bind_text(st, "?2", imports[i]);
            aimee_pg_bind_int64(st, "?3", imp_sys[i]);
            aimee_pg_step(st, err, sizeof(err));
            aimee_pg_reset(st);
         }
         free(imports[i]);
      }
      if (st)
         aimee_pg_finalize(st);
   }

   /* Routes (stored as terms with kind='route'). */
   {
      char *routes[64];
      int n = extract_routes(ext, content, routes, 64);
      aimee_pg_stmt_t *st = aimee_pg_prepare(
          conn, "INSERT INTO terms (file_id, name, kind, line) VALUES (?1, ?2, 'route', 0)", err,
          sizeof(err));
      for (int i = 0; i < n; i++)
      {
         if (st)
         {
            aimee_pg_bind_int64(st, "?1", file_id);
            aimee_pg_bind_text(st, "?2", routes[i]);
            aimee_pg_step(st, err, sizeof(err));
            aimee_pg_reset(st);
         }
         free(routes[i]);
      }
      if (st)
         aimee_pg_finalize(st);
   }

   /* Definitions. */
   {
      definition_t defs[CI_MAX_DEFS];
      int n = extract_definitions(ext, content, defs, CI_MAX_DEFS);
      /* H0a: coarse kind stays 'definition' (the ~10 kind='definition' consumers are
       * unchanged); the extractor's granular kind goes to the new def_kind column,
       * which the cross-repo resolver reads for §5 kind-eligibility. The extractor
       * emits a granular kind for C today and 'definition' (treated as unknown ->
       * eligible) for languages not yet upgraded. */
      aimee_pg_stmt_t *st = aimee_pg_prepare(
          conn,
          "INSERT INTO terms (file_id, name, kind, def_kind, line) VALUES (?1, ?2, "
          "'definition', ?3, ?4)",
          err, sizeof(err));
      for (int i = 0; i < n; i++)
      {
         if (st)
         {
            aimee_pg_bind_int64(st, "?1", file_id);
            aimee_pg_bind_text(st, "?2", defs[i].name);
            aimee_pg_bind_text(st, "?3", defs[i].kind);
            aimee_pg_bind_int(st, "?4", defs[i].line);
            aimee_pg_step(st, err, sizeof(err));
            aimee_pg_reset(st);
         }
      }
      if (st)
         aimee_pg_finalize(st);
   }

   /* Call references. */
   {
      call_ref_t calls[512];
      int n = extract_calls(ext, content, calls, 512);
      aimee_pg_stmt_t *st = aimee_pg_prepare(
          conn, "INSERT INTO code_calls (file_id, caller, callee, line) VALUES (?1, ?2, ?3, ?4)",
          err, sizeof(err));
      for (int i = 0; i < n; i++)
      {
         if (st)
         {
            aimee_pg_bind_int64(st, "?1", file_id);
            aimee_pg_bind_text(st, "?2", calls[i].caller);
            aimee_pg_bind_text(st, "?3", calls[i].callee);
            aimee_pg_bind_int(st, "?4", calls[i].line);
            aimee_pg_step(st, err, sizeof(err));
            aimee_pg_reset(st);
         }
      }
      if (st)
         aimee_pg_finalize(st);
   }

   if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
   {
      LOG_WARN(CI_LOG_TAG, "COMMIT failed: %s", err);
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
   }
}

/* CSS migration assistant (WP-C/D): build the style graph for .css files and the
 * component->style join for markup files. Called AFTER ci_replace_file_data
 * (whose transaction has committed) because db2_css_graph_replace /
 * db2_css_component_resolve run their own transactions — never nest them. Gated
 * by css_style_graph_enabled (read once per scan). Plain CSS only (.css); SCSS is
 * indexed from compiled output. Thread-safe (heap token buffer — the KB indexer
 * runs in worker threads). */
static void ci_css_index_file(int64_t file_id, const char *ext, const char *content, int css_on)
{
   if (!css_on || !ext || !content)
      return;
   size_t len = strlen(content);
   if (strcmp(ext, ".css") == 0)
   {
      css_stylesheet_t *ss = css_analyze(content, len);
      if (ss)
      {
         (void)db2_css_graph_replace(file_id, ss->rules, ss->rule_count);
         css_stylesheet_free(ss);
      }
      return;
   }
   if (strcmp(ext, ".tsx") == 0 || strcmp(ext, ".jsx") == 0 || strcmp(ext, ".ts") == 0 ||
       strcmp(ext, ".js") == 0 || strcmp(ext, ".html") == 0 || strcmp(ext, ".vue") == 0 ||
       strcmp(ext, ".svelte") == 0)
   {
      char(*tokens)[CSS_CLASS_TOKEN_MAX] = malloc((size_t)512 * CSS_CLASS_TOKEN_MAX);
      if (!tokens)
         return;
      int nt = css_extract_class_tokens(content, len, tokens, 512);
      if (nt > 0)
         (void)db2_css_component_resolve(file_id, tokens, nt);
      free(tokens);
   }
}

/* ---- File system traversal ------------------------------------- */

typedef struct
{
   char **paths;
   int count;
   int max;
} ci_file_list_t;

typedef struct
{
   char paths[64][256];
   int count;
} ci_exclusion_list_t;

static int ci_path_component_is_hidden(const char *component, size_t len)
{
   if (len == 0)
      return 0;
   return component[0] == '.';
}

static int ci_path_has_hidden_component(const char *path)
{
   const char *p = path;
   while (*p == '/')
      p++;

   const char *start = p;
   for (;;)
   {
      if (*p == '/' || *p == '\0')
      {
         if (ci_path_component_is_hidden(start, (size_t)(p - start)))
            return 1;
         if (*p == '\0')
            break;
         p++;
         start = p;
      }
      else
      {
         p++;
      }
   }

   return 0;
}

static int ci_file_list_append(ci_file_list_t *list, const char *path)
{
   if (list->count >= list->max)
   {
      int next = list->max > 0 ? list->max * 2 : 1024;
      char **grown = realloc(list->paths, sizeof(*grown) * (size_t)next);
      if (!grown)
         return -1;
      list->paths = grown;
      list->max = next;
   }

   list->paths[list->count] = strdup(path);
   if (!list->paths[list->count])
      return -1;
   list->count++;
   return 0;
}

static void ci_purge_hidden_paths(int64_t project_id)
{
   (void)db2_code_index_purge_files_matching(project_id, ".%");
   (void)db2_code_index_purge_files_matching(project_id, "%/.%");
}

static void ci_purge_build_exclusions(int64_t project_id, const ci_exclusion_list_t *exclusions)
{
   char pattern[MAX_PATH_LEN];
   for (int i = 0; i < exclusions->count; i++)
   {
      const char *exclude = exclusions->paths[i];
      size_t len = strlen(exclude);
      if (len > 0 && exclude[len - 1] == '/')
      {
         snprintf(pattern, sizeof(pattern), "%s%%", exclude);
         (void)db2_code_index_purge_files_matching(project_id, pattern);
      }
      else
      {
         (void)db2_code_index_purge_files_matching(project_id, exclude);
      }
   }
}

static int ci_ascii_contains(const char *haystack, const char *needle)
{
   size_t needle_len = strlen(needle);
   if (needle_len == 0)
      return 1;
   for (const char *p = haystack; *p; p++)
   {
      size_t i = 0;
      while (i < needle_len && p[i] &&
             toupper((unsigned char)p[i]) == toupper((unsigned char)needle[i]))
         i++;
      if (i == needle_len)
         return 1;
   }
   return 0;
}

static int ci_key_matches_output_subject(const char *key, const char *subject)
{
   size_t subject_len = strlen(subject);
   for (const char *p = key; *p; p++)
   {
      if (p > key && (isalnum((unsigned char)p[-1]) || p[-1] == '-'))
         continue;

      size_t i = 0;
      while (i < subject_len && p[i] &&
             toupper((unsigned char)p[i]) == toupper((unsigned char)subject[i]))
         i++;
      if (i != subject_len)
         continue;

      const char *after = p + subject_len;
      if (*after == '\0' || !isalnum((unsigned char)*after) || ci_ascii_contains(after, "DIR") ||
          ci_ascii_contains(after, "PATH") || ci_ascii_contains(after, "FILE"))
         return 1;
   }
   return 0;
}

static int ci_build_key_is_output(const char *key)
{
   int subject =
       ci_key_matches_output_subject(key, "BUILD") || ci_key_matches_output_subject(key, "OBJ") ||
       ci_key_matches_output_subject(key, "OUT") || ci_key_matches_output_subject(key, "OUTPUT") ||
       ci_key_matches_output_subject(key, "DIST") || ci_key_matches_output_subject(key, "TARGET") ||
       ci_key_matches_output_subject(key, "GEN") || ci_key_matches_output_subject(key, "GENERATED");
   int shape = ci_ascii_contains(key, "DIR") || ci_ascii_contains(key, "PATH") ||
               ci_ascii_contains(key, "FILE");
   return subject && shape;
}

static void ci_trim_token(char *token)
{
   while (*token == '"' || *token == '\'' || *token == '`')
      memmove(token, token + 1, strlen(token));
   size_t len = strlen(token);
   while (len > 0 && (token[len - 1] == '"' || token[len - 1] == '\'' || token[len - 1] == '`' ||
                      token[len - 1] == ';' || token[len - 1] == ',' || token[len - 1] == ')'))
      token[--len] = '\0';
}

static void ci_exclusions_add(ci_exclusion_list_t *exclusions, const char *base_dir,
                              const char *token, int force_dir)
{
   if (exclusions->count >= (int)(sizeof(exclusions->paths) / sizeof(exclusions->paths[0])) ||
       !token || !*token || token[0] == '/' || strstr(token, "://") || strchr(token, '$') ||
       strchr(token, '*') || strchr(token, '%') || strchr(token, '&'))
      return;

   while (strncmp(token, "./", 2) == 0)
      token += 2;
   if (!*token || ci_path_has_hidden_component(token))
      return;

   char rel[256];
   if (base_dir && *base_dir)
      snprintf(rel, sizeof(rel), "%s/%s", base_dir, token);
   else
      snprintf(rel, sizeof(rel), "%s", token);

   const char *slash = strrchr(rel, '/');
   const char *dot = strrchr(rel, '.');
   int is_dir = force_dir || !dot || (slash && dot < slash);
   if (is_dir)
   {
      size_t len = strlen(rel);
      if (len + 1 < sizeof(rel) && rel[len - 1] != '/')
      {
         rel[len] = '/';
         rel[len + 1] = '\0';
      }
   }

   for (int i = 0; i < exclusions->count; i++)
      if (strcmp(exclusions->paths[i], rel) == 0)
         return;
   snprintf(exclusions->paths[exclusions->count++], sizeof(exclusions->paths[0]), "%s", rel);
}

static int ci_path_is_build_excluded(const ci_exclusion_list_t *exclusions, const char *rel_path)
{
   for (int i = 0; i < exclusions->count; i++)
   {
      const char *exclude = exclusions->paths[i];
      size_t len = strlen(exclude);
      if (len > 0 && exclude[len - 1] == '/')
      {
         if (strncmp(rel_path, exclude, len) == 0)
            return 1;
      }
      else if (strcmp(rel_path, exclude) == 0)
      {
         return 1;
      }
   }
   return 0;
}

static void ci_parse_build_assignment(ci_exclusion_list_t *exclusions, const char *base_dir,
                                      const char *key, char *value)
{
   if (!ci_build_key_is_output(key))
      return;

   char *comment = strchr(value, '#');
   if (comment)
      *comment = '\0';

   int force_dir = ci_ascii_contains(key, "DIR") || ci_ascii_contains(key, "PATH");
   char *save = NULL;
   for (char *tok = strtok_r(value, " \t\r\n", &save); tok; tok = strtok_r(NULL, " \t\r\n", &save))
   {
      ci_trim_token(tok);
      ci_exclusions_add(exclusions, base_dir, tok, force_dir);
   }
}

static void ci_parse_build_manifest(const char *root, const char *path,
                                    ci_exclusion_list_t *exclusions)
{
   const char *rel = path;
   size_t root_len = strlen(root);
   if (strncmp(path, root, root_len) == 0)
   {
      rel = path + root_len;
      if (*rel == '/')
         rel++;
   }

   char base_dir[256] = "";
   snprintf(base_dir, sizeof(base_dir), "%s", rel);
   char *slash = strrchr(base_dir, '/');
   if (slash)
      *slash = '\0';
   else
      base_dir[0] = '\0';

   FILE *f = fopen(path, "r");
   if (!f)
      return;

   char line[1024];
   while (fgets(line, sizeof(line), f))
   {
      char *eq = strchr(line, '=');
      if (!eq)
         continue;
      *eq = '\0';

      char *key = line;
      while (*key == ' ' || *key == '\t')
         key++;
      char *end = key + strlen(key);
      while (end > key && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == ':' || end[-1] == '?' ||
                           end[-1] == '+'))
         *--end = '\0';

      ci_parse_build_assignment(exclusions, base_dir, key, eq + 1);
   }
   fclose(f);
}

static void ci_collect_build_exclusions(const char *root, ci_exclusion_list_t *exclusions)
{
   char *esc = shell_escape(root);
   char cmd[MAX_PATH_LEN * 2 + 512];
   int rc;
   snprintf(cmd, sizeof(cmd), "git -C %s rev-parse --is-inside-work-tree 2>/dev/null", esc);
   char *probe = run_cmd(cmd, &rc);
   int in_git = (rc == 0 && probe && strncmp(probe, "true", 4) == 0);
   free(probe);

   char *out = NULL;
   if (in_git)
   {
      snprintf(cmd, sizeof(cmd),
               "git -C %s ls-files --cached --others --exclude-standard 2>/dev/null", esc);
      out = run_cmd(cmd, &rc);
   }
   else
   {
      snprintf(cmd, sizeof(cmd),
               "find %s -maxdepth 4 -type f \\( -name Makefile -o -name makefile"
               " -o -name GNUmakefile -o -name '*.mk' -o -name CMakeLists.txt"
               " -o -name package.json -o -name pyproject.toml -o -name Cargo.toml \\)"
               " ! -name '.*' ! -path '*/.*/*' 2>/dev/null",
               esc);
      out = run_cmd(cmd, &rc);
   }
   free(esc);
   if (!out)
      return;

   size_t root_len = strlen(root);
   for (char *line = out, *nl; line && *line; line = nl ? nl + 1 : NULL)
   {
      nl = strchr(line, '\n');
      if (nl)
         *nl = '\0';
      if (!line[0] || ci_path_has_hidden_component(line))
         continue;

      const char *base = strrchr(line, '/');
      base = base ? base + 1 : line;
      if (strcmp(base, "Makefile") != 0 && strcmp(base, "makefile") != 0 &&
          strcmp(base, "GNUmakefile") != 0 && !strstr(base, ".mk") &&
          strcmp(base, "CMakeLists.txt") != 0 && strcmp(base, "package.json") != 0 &&
          strcmp(base, "pyproject.toml") != 0 && strcmp(base, "Cargo.toml") != 0)
         continue;

      char full[MAX_PATH_LEN];
      const char *path = line;
      if (in_git)
      {
         snprintf(full, sizeof(full), "%.*s/%s", (int)root_len, root, line);
         path = full;
      }
      ci_parse_build_manifest(root, path, exclusions);
   }
   free(out);
}

/* Extensions we know are binary blobs and should never be indexed.
 * Anything not on this list is given a chance — content sniffing in
 * ci_read_file_content will reject anything with embedded NULs. */
static int ci_is_binary_extension(const char *ext)
{
   static const char *bin_exts[] = {
       /* images (svg is text, kept) */
       ".png",
       ".jpg",
       ".jpeg",
       ".gif",
       ".bmp",
       ".ico",
       ".webp",
       ".tiff",
       ".tif",
       ".heic",
       ".avif",
       ".psd",
       ".ai",
       /* archives */
       ".zip",
       ".tar",
       ".gz",
       ".tgz",
       ".bz2",
       ".tbz2",
       ".xz",
       ".txz",
       ".7z",
       ".rar",
       ".zst",
       ".lz4",
       ".lzma",
       ".jar",
       ".war",
       ".ear",
       ".whl",
       ".egg",
       /* compiled artefacts */
       ".o",
       ".a",
       ".so",
       ".dylib",
       ".dll",
       ".exe",
       ".obj",
       ".lib",
       ".pdb",
       ".class",
       ".pyc",
       ".pyo",
       ".wasm",
       /* media */
       ".mp3",
       ".mp4",
       ".m4a",
       ".m4v",
       ".avi",
       ".mov",
       ".mkv",
       ".wav",
       ".ogg",
       ".oga",
       ".ogv",
       ".flac",
       ".webm",
       ".opus",
       ".mid",
       ".midi",
       ".aiff",
       /* documents */
       ".pdf",
       ".doc",
       ".docx",
       ".xls",
       ".xlsx",
       ".ppt",
       ".pptx",
       ".odt",
       ".ods",
       ".odp",
       ".pages",
       ".numbers",
       ".key",
       /* fonts */
       ".ttf",
       ".otf",
       ".woff",
       ".woff2",
       ".eot",
       /* databases / containers */
       ".db",
       ".sqlite",
       ".sqlite3",
       ".mdb",
       ".dbf",
       /* misc binary */
       ".bin",
       ".dat",
       ".iso",
       ".dmg",
       ".deb",
       ".rpm",
       ".pkg",
       ".msi",
       ".apk",
       ".ipa",
       NULL,
   };
   if (!ext || !*ext)
      return 0;
   for (int i = 0; bin_exts[i]; i++)
      if (strcasecmp(ext, bin_exts[i]) == 0)
         return 1;
   return 0;
}

static void ci_collect_text_files(const char *root, ci_file_list_t *list)
{
   ci_exclusion_list_t build_exclusions = {0};
   ci_collect_build_exclusions(root, &build_exclusions);

   char *esc = shell_escape(root);
   char cmd[MAX_PATH_LEN * 2 + 512];
   int rc;
   snprintf(cmd, sizeof(cmd), "git -C %s rev-parse --is-inside-work-tree 2>/dev/null", esc);
   char *probe = run_cmd(cmd, &rc);
   int in_git = (rc == 0 && probe && strncmp(probe, "true", 4) == 0);
   free(probe);

   char *out = NULL;
   if (in_git)
   {
      snprintf(cmd, sizeof(cmd),
               "git -C %s ls-files --cached --others --exclude-standard 2>/dev/null", esc);
      out = run_cmd(cmd, &rc);
   }
   else
   {
      snprintf(cmd, sizeof(cmd),
               "find %s -type f"
               " ! -name '.*' ! -path '*/.*/*'"
               " -size -%dc 2>/dev/null",
               esc, MAX_FILE_SIZE + 1);
      out = run_cmd(cmd, &rc);
   }
   free(esc);
   if (!out)
      return;

   size_t root_len = strlen(root);
   for (char *line = out, *nl; line && *line; line = nl ? nl + 1 : NULL)
   {
      nl = strchr(line, '\n');
      if (nl)
         *nl = '\0';
      if (!line[0])
         continue;

      char full[MAX_PATH_LEN];
      char relbuf[MAX_PATH_LEN];
      const char *abs_path = line;
      const char *policy_path = line;
      if (in_git)
      {
         snprintf(full, sizeof(full), "%.*s/%s", (int)root_len, root, line);
         abs_path = full;
         policy_path = line;
      }
      else if (strncmp(line, root, root_len) == 0)
      {
         const char *rel = line + root_len;
         if (*rel == '/')
            rel++;
         snprintf(relbuf, sizeof(relbuf), "%s", rel);
         policy_path = relbuf;
      }

      if (ci_path_has_hidden_component(policy_path))
         continue;
      if (ci_path_is_build_excluded(&build_exclusions, policy_path))
         continue;
      if (ci_is_binary_extension(ci_get_extension(abs_path)))
         continue;
      if (ci_file_list_append(list, abs_path) != 0)
         break;
   }
   free(out);
}

/* Read a file into a NUL-terminated buffer. Rejects files that contain
 * embedded NULs in the first 4 KiB — those are almost always binary
 * formats slipping past the extension allowlist (PDFs with .txt
 * extensions, sqlite blobs, etc.) and storing them in file_contents
 * would either truncate or pollute the index. */
static char *ci_read_file_content(const char *path, size_t *out_len)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long len = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (len <= 0 || len > MAX_FILE_SIZE)
   {
      fclose(f);
      return NULL;
   }
   char *buf = malloc((size_t)len + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t n = fread(buf, 1, (size_t)len, f);
   buf[n] = '\0';
   fclose(f);

   size_t sniff = n < 4096 ? n : 4096;
   if (memchr(buf, '\0', sniff) != NULL)
   {
      free(buf);
      return NULL;
   }

   if (out_len)
      *out_len = n;
   return buf;
}

/* ---- Public API: scan ----------------------------------------- */

int canonical_index_scan_project(const char *name, const char *root, int force, int *inspected_out)
{
   if (inspected_out)
      *inspected_out = 0;

   void *conn = ci_conn();
   if (!conn)
      return -1;

   /* CSS style-graph write path is opt-in; read once per scan (WP-C). */
   config_t css_cfg;
   int css_on = (config_load(&css_cfg) == 0) && css_cfg.css_style_graph_enabled;

   char abs_root[MAX_PATH_LEN];
   if (realpath(root, abs_root) == NULL)
      snprintf(abs_root, sizeof(abs_root), "%s", root);

   if (ci_path_has_hidden_component(abs_root))
   {
      fprintf(stderr, "aimee index: refusing to scan excluded root: %s\n", abs_root);
      return -1;
   }

   int64_t project_id = ci_upsert_project(conn, name, abs_root);
   if (project_id < 0)
      return -1;

   ci_exclusion_list_t build_exclusions = {0};
   ci_collect_build_exclusions(abs_root, &build_exclusions);
   ci_purge_hidden_paths(project_id);
   ci_purge_build_exclusions(project_id, &build_exclusions);

   char ts[32];
   now_utc(ts, sizeof(ts));

   ci_file_list_t list = {0};
   ci_collect_text_files(abs_root, &list);

   if (inspected_out)
      *inspected_out = list.count;

   int scanned = 0;
   for (int i = 0; i < list.count; i++)
   {
      const char *full = list.paths[i];
      const char *rel = full + strlen(abs_root);
      if (*rel == '/')
         rel++;

      struct stat st;
      if (stat(full, &st) != 0)
      {
         free(list.paths[i]);
         continue;
      }
      if (!force && !ci_file_modified_since(conn, project_id, rel, st.st_mtime))
      {
         free(list.paths[i]);
         continue;
      }

      size_t content_len;
      char *content = ci_read_file_content(full, &content_len);
      if (!content)
      {
         free(list.paths[i]);
         continue;
      }

      int64_t file_id = ci_upsert_file(conn, project_id, rel, ts);
      if (file_id < 0)
      {
         free(content);
         free(list.paths[i]);
         continue;
      }

      const char *ext = ci_get_extension(full);
      ci_replace_file_data(conn, file_id, ext, content);
      ci_css_index_file(file_id, ext, content, css_on);

      free(content);
      free(list.paths[i]);
      scanned++;
   }

   free(list.paths);
   return scanned;
}

int canonical_index_scan_files(const char *name, const char *root_label,
                               const canonical_index_file_input_t *files, int file_count, int force,
                               int *inspected_out)
{
   (void)force;
   if (inspected_out)
      *inspected_out = 0;
   if (!name || !name[0] || !files || file_count < 0)
      return -1;

   /* CSS style-graph write path is opt-in; read once per scan (WP-C). */
   config_t css_cfg;
   int css_on = (config_load(&css_cfg) == 0) && css_cfg.css_style_graph_enabled;

   void *conn = ci_conn();
   if (!conn)
      return -1;

   int64_t project_id =
       ci_upsert_project(conn, name, root_label && root_label[0] ? root_label : "remote");
   if (project_id < 0)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   int inspected = 0;
   int scanned = 0;
   for (int i = 0; i < file_count; i++)
   {
      const char *rel = files[i].rel_path;
      const char *content = files[i].content;
      if (!rel || !rel[0] || rel[0] == '/' || ci_path_has_hidden_component(rel) || !content)
         continue;

      inspected++;
      int64_t file_id = ci_upsert_file(conn, project_id, rel, ts);
      if (file_id < 0)
         continue;

      const char *css_ext = ci_get_extension(rel);
      ci_replace_file_data(conn, file_id, css_ext, content);
      ci_css_index_file(file_id, css_ext, content, css_on);
      scanned++;
   }

   if (inspected_out)
      *inspected_out = inspected;
   return scanned;
}

/* ---- Public API: read ----------------------------------------- */

int canonical_index_list_projects(project_info_t *out, int max)
{
   void *conn = ci_conn();
   if (!conn)
      return -1;

   char err[CI_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT name, root, scanned_at FROM projects"
                                          " WHERE root NOT LIKE '%/.%'"
                                          " ORDER BY name",
                                          err, sizeof(err));
   if (!st)
      return -1;
   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *n = aimee_pg_column_text(st, 0);
      const char *r = aimee_pg_column_text(st, 1);
      const char *s = aimee_pg_column_text(st, 2);
      snprintf(out[count].name, sizeof(out[count].name), "%s", n ? n : "");
      snprintf(out[count].root, sizeof(out[count].root), "%s", r ? r : "");
      snprintf(out[count].scanned_at, sizeof(out[count].scanned_at), "%s", s ? s : "");
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}

/* Build a LIKE pattern from identifier, escaping SQL wildcards.
 * C identifiers contain '_' which LIKE treats as a single-char wildcard,
 * so we escape '%', '_', and '\' before adding caller-controlled '%'
 * wildcards. leading_pct/trailing_pct select prefix vs substring matching.
 * The companion SQL must use ESCAPE '\\' so backslash is the escape char. */
static void ci_make_like_pattern(const char *id, int leading_pct, int trailing_pct, char *buf,
                                 size_t buflen)
{
   size_t j = 0;
   if (leading_pct && j + 1 < buflen)
      buf[j++] = '%';
   for (size_t i = 0; id[i] && j + 3 < buflen; i++)
   {
      if (id[i] == '%' || id[i] == '_' || id[i] == '\\')
         buf[j++] = '\\';
      buf[j++] = (unsigned char)id[i];
   }
   if (trailing_pct && j + 1 < buflen)
      buf[j++] = '%';
   buf[j] = '\0';
}

static const char *ci_find_sql = "SELECT p.name, f.path, t.line, t.kind"
                                 " FROM terms t"
                                 " JOIN files f ON f.id = t.file_id"
                                 " JOIN projects p ON p.id = f.project_id"
                                 " WHERE t.name = ?1"
                                 "   AND f.path NOT LIKE '.%'"
                                 "   AND f.path NOT LIKE '%/.%'"
                                 "   AND p.root NOT LIKE '%/.%'"
                                 " GROUP BY p.name, f.path, t.line, t.kind"
                                 " ORDER BY CASE WHEN t.kind = 'definition' THEN 0 ELSE 1 END,"
                                 " p.name, f.path"
                                 " LIMIT ?2";

static const char *ci_find_like_sql = "SELECT p.name, f.path, t.line, t.kind"
                                      " FROM terms t"
                                      " JOIN files f ON f.id = t.file_id"
                                      " JOIN projects p ON p.id = f.project_id"
                                      " WHERE t.name LIKE ?1 ESCAPE '\\'"
                                      "   AND f.path NOT LIKE '.%'"
                                      "   AND f.path NOT LIKE '%/.%'"
                                      "   AND p.root NOT LIKE '%/.%'"
                                      " GROUP BY p.name, f.path, t.line, t.kind"
                                      " ORDER BY CASE WHEN t.kind = 'definition' THEN 0 ELSE 1 END,"
                                      " p.name, f.path"
                                      " LIMIT ?2";

static int ci_drain_term_hits(aimee_pg_stmt_t *st, term_hit_t *out, int max, char *err,
                              size_t errlen)
{
   int count = 0;
   while (count < max && aimee_pg_step(st, err, errlen) == AIMEE_PG_ROW)
   {
      const char *p = aimee_pg_column_text(st, 0);
      const char *f = aimee_pg_column_text(st, 1);
      int line = aimee_pg_column_int(st, 2);
      const char *k = aimee_pg_column_text(st, 3);
      snprintf(out[count].project, sizeof(out[count].project), "%s", p ? p : "");
      snprintf(out[count].file_path, sizeof(out[count].file_path), "%s", f ? f : "");
      out[count].line = line;
      snprintf(out[count].kind, sizeof(out[count].kind), "%s", k ? k : "");
      count++;
   }
   return count;
}

int canonical_index_find(const char *identifier, term_hit_t *out, int max)
{
   void *conn = ci_conn();
   if (!conn)
      return -1;

   char err[CI_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, ci_find_sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", identifier);
   aimee_pg_bind_int(st, "?2", max);
   int count = ci_drain_term_hits(st, out, max, err, sizeof(err));
   aimee_pg_finalize(st);

   /* Two LIKE fallbacks if exact match was empty: prefix first (preserves
    * "find aimee_db_t" precision), then substring (so "find qdrant" still
    * finds kb_test_qdrant_post_handler). Each tier only fires when the
    * previous one returned zero rows. */
   for (int tier = 0; count == 0 && tier < 2; tier++)
   {
      char pattern[512];
      ci_make_like_pattern(identifier, tier == 1, 1, pattern, sizeof(pattern));
      aimee_pg_stmt_t *st2 = aimee_pg_prepare(conn, ci_find_like_sql, err, sizeof(err));
      if (!st2)
         break;
      aimee_pg_bind_text(st2, "?1", pattern);
      aimee_pg_bind_int(st2, "?2", max);
      count = ci_drain_term_hits(st2, out, max, err, sizeof(err));
      aimee_pg_finalize(st2);
   }
   return count;
}

int canonical_index_blast_radius(const char *project, const char *file_path, blast_radius_t *out)
{
   void *conn = ci_conn();
   if (!conn)
      return -1;

   memset(out, 0, sizeof(*out));
   snprintf(out->file, sizeof(out->file), "%s", file_path);

   int64_t project_id = ci_resolve_project_id(conn, project);
   if (project_id < 0)
      return -1;

   int64_t file_id = ci_resolve_file_id(conn, project_id, file_path);
   char err[CI_ERRBUF] = "";

   /* Dependents: files in the project whose imports match this file's path. */
   {
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                             "SELECT DISTINCT f.path FROM file_imports fi"
                                             " JOIN files f ON f.id = fi.file_id"
                                             " WHERE f.project_id = ?1 AND fi.name LIKE ?2",
                                             err, sizeof(err));
      if (st)
      {
         char pattern[MAX_PATH_LEN];
         snprintf(pattern, sizeof(pattern), "%%%s%%", file_path);
         aimee_pg_bind_int64(st, "?1", project_id);
         aimee_pg_bind_text(st, "?2", pattern);
         while (out->dependent_count < CI_MAX_DEPS &&
                aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
         {
            const char *p = aimee_pg_column_text(st, 0);
            if (p && strcmp(p, file_path) != 0)
            {
               snprintf(out->dependents[out->dependent_count], MAX_PATH_LEN, "%s", p);
               out->dependent_count++;
            }
         }
         aimee_pg_finalize(st);
      }
   }

   /* Dependencies: imports declared by this file. */
   if (file_id >= 0)
   {
      aimee_pg_stmt_t *st = aimee_pg_prepare(
          conn, "SELECT DISTINCT name FROM file_imports WHERE file_id = ?1", err, sizeof(err));
      if (st)
      {
         aimee_pg_bind_int64(st, "?1", file_id);
         while (out->dependency_count < CI_MAX_DEPS &&
                aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
         {
            const char *t = aimee_pg_column_text(st, 0);
            if (t)
            {
               snprintf(out->dependencies[out->dependency_count], MAX_PATH_LEN, "%s", t);
               out->dependency_count++;
            }
         }
         aimee_pg_finalize(st);
      }
   }

   return 0;
}

int canonical_index_structure(const char *project, const char *file_path, definition_t *out,
                              int max)
{
   void *conn = ci_conn();
   if (!conn)
      return -1;

   int64_t project_id = ci_resolve_project_id(conn, project);
   if (project_id < 0)
      return 0;
   int64_t file_id = ci_resolve_file_id(conn, project_id, file_path);
   if (file_id < 0)
      return 0;

   char err[CI_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT name, kind, line FROM terms"
                                          " WHERE file_id = ?1 AND kind = 'definition'"
                                          " ORDER BY line",
                                          err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", file_id);

   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *n = aimee_pg_column_text(st, 0);
      const char *k = aimee_pg_column_text(st, 1);
      int line = aimee_pg_column_int(st, 2);
      snprintf(out[count].name, sizeof(out[count].name), "%s", n ? n : "");
      snprintf(out[count].kind, sizeof(out[count].kind), "%s", k ? k : "");
      out[count].line = line;
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}

int canonical_index_find_callers(const char *project, const char *symbol, caller_hit_t *out,
                                 int max)
{
   void *conn = ci_conn();
   if (!conn)
      return -1;

   char err[CI_ERRBUF] = "";
   const char *sql_all = "SELECT p.name, f.path, cc.caller, cc.line"
                         " FROM code_calls cc"
                         " JOIN files f ON f.id = cc.file_id"
                         " JOIN projects p ON p.id = f.project_id"
                         " WHERE cc.callee = ?1"
                         "   AND f.path NOT LIKE '.%'"
                         "   AND f.path NOT LIKE '%/.%'"
                         "   AND p.root NOT LIKE '%/.%'"
                         " ORDER BY p.name, f.path, cc.line"
                         " LIMIT ?2";
   const char *sql_proj = "SELECT p.name, f.path, cc.caller, cc.line"
                          " FROM code_calls cc"
                          " JOIN files f ON f.id = cc.file_id"
                          " JOIN projects p ON p.id = f.project_id"
                          " WHERE cc.callee = ?1 AND p.name = ?2"
                          "   AND f.path NOT LIKE '.%'"
                          "   AND f.path NOT LIKE '%/.%'"
                          "   AND p.root NOT LIKE '%/.%'"
                          " ORDER BY f.path, cc.line"
                          " LIMIT ?3";

   aimee_pg_stmt_t *st;
   if (project && project[0])
   {
      st = aimee_pg_prepare(conn, sql_proj, err, sizeof(err));
      if (!st)
         return 0;
      aimee_pg_bind_text(st, "?1", symbol);
      aimee_pg_bind_text(st, "?2", project);
      aimee_pg_bind_int(st, "?3", max);
   }
   else
   {
      st = aimee_pg_prepare(conn, sql_all, err, sizeof(err));
      if (!st)
         return 0;
      aimee_pg_bind_text(st, "?1", symbol);
      aimee_pg_bind_int(st, "?2", max);
   }

   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *pname = aimee_pg_column_text(st, 0);
      const char *fpath = aimee_pg_column_text(st, 1);
      const char *caller = aimee_pg_column_text(st, 2);
      int line = aimee_pg_column_int(st, 3);
      snprintf(out[count].project, sizeof(out[count].project), "%s", pname ? pname : "");
      snprintf(out[count].file_path, sizeof(out[count].file_path), "%s", fpath ? fpath : "");
      snprintf(out[count].caller, sizeof(out[count].caller), "%s", caller ? caller : "");
      out[count].line = line;
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}

int canonical_index_project_stats(const char *project, int *files_out, int *defs_out)
{
   if (files_out)
      *files_out = 0;
   if (defs_out)
      *defs_out = 0;
   if (!project || !project[0])
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "SELECT"
                            " (SELECT COUNT(*) FROM files f"
                            "  JOIN projects p ON p.id = f.project_id"
                            "  WHERE p.name = ?1"
                            "    AND f.path NOT LIKE '.%'"
                            "    AND f.path NOT LIKE '%/.%'"
                            "    AND p.root NOT LIKE '%/.%'),"
                            " (SELECT COUNT(*) FROM terms t"
                            "  JOIN files f ON f.id = t.file_id"
                            "  JOIN projects p ON p.id = f.project_id"
                            "  WHERE p.name = ?2 AND t.kind = 'definition'"
                            "    AND f.path NOT LIKE '.%'"
                            "    AND f.path NOT LIKE '%/.%'"
                            "    AND p.root NOT LIKE '%/.%')";
   char err[CI_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", project);
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (files_out)
         *files_out = aimee_pg_column_int(st, 0);
      if (defs_out)
         *defs_out = aimee_pg_column_int(st, 1);
   }
   aimee_pg_finalize(st);
   return 0;
}

int canonical_index_project_lang_breakdown(const char *project, char *buf, size_t bufsz)
{
   if (!buf || bufsz < 3)
      return -1;
   buf[0] = '[';
   buf[1] = ']';
   buf[2] = '\0';
   if (!project || !project[0])
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "SELECT COALESCE(lower(substring(f.path FROM '\\.([^./]+)$')), '') AS ext,"
       "       COUNT(*) AS cnt"
       " FROM files f"
       " JOIN projects p ON p.id = f.project_id"
       " WHERE p.name = ?1"
       "   AND f.path NOT LIKE '%/'"
       "   AND f.path NOT LIKE '.%'"
       "   AND f.path NOT LIKE '%/.%'"
       "   AND p.root NOT LIKE '%/.%'"
       " GROUP BY ext"
       " ORDER BY cnt DESC"
       " LIMIT 8";
   char err[CI_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);

   size_t pos = 0;
   /* Write into a temporary buffer and then copy into buf. */
   char tmp[1024];
   tmp[pos++] = '[';

   int first = 1;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *ext = aimee_pg_column_text(st, 0);
      int cnt = aimee_pg_column_int(st, 1);
      if (!ext || !ext[0])
         ext = "other";
      int n = snprintf(tmp + pos, sizeof(tmp) - pos, "%s{\"lang\":\"%s\",\"count\":%d}",
                       first ? "" : ",", ext, cnt);
      if (n > 0 && (size_t)n < sizeof(tmp) - pos)
         pos += (size_t)n;
      first = 0;
      if (pos + 32 >= sizeof(tmp))
         break;
   }
   aimee_pg_finalize(st);

   if (pos + 2 > sizeof(tmp))
      pos = sizeof(tmp) - 2;
   tmp[pos++] = ']';
   tmp[pos] = '\0';

   snprintf(buf, bufsz, "%s", tmp);
   return 0;
}

int canonical_index_code_search(const char *query, const char *project, code_search_hit_t *out,
                                int max, int enrich)
{
   /* Forward to db2_code_index — same SQL, same connection. */
   return db2_code_index_code_search(query, project, out, max, enrich);
}
