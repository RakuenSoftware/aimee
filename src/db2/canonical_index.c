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
#include "entity_edges.h"     /* co_edited backfill: edge upsert / co_targets read */
#include "index.h"            /* cochange_pairs_for_commit / cochange_is_hex_sha */
#include "kb_runtime_state.h" /* db2_kb_purge_fence_active: commit-point fence check */
#include "modules/memory/memory_ontology.h" /* REL_CO_EDITED / NODE_FILE */

#include "aimee.h"
#include "db_postgres.h"
#include "log.h"
#include "util.h"

#include <ctype.h>
#include <stdatomic.h>
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
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT id FROM projects WHERE name = ?1 AND lifecycle_state = 'current'", err,
       sizeof(err));
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
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT f.id FROM files f JOIN projects p ON p.id=f.project_id"
                        " WHERE f.project_id = ?1 AND f.path = ?2"
                        " AND p.lifecycle_state='current' AND f.generation=p.current_generation",
                        err, sizeof(err));
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
   (void)conn;
   return db2_code_index_project_upsert(name, root);
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
       "INSERT INTO files (project_id, generation, path, scanned_at, language, vendored) "
       "SELECT ?1, current_generation, ?2, ?3, ?4, ?5 FROM projects "
       "WHERE id=?1 AND lifecycle_state='current' "
       "ON CONFLICT (project_id, generation, path) DO UPDATE SET scanned_at = EXCLUDED.scanned_at, "
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

/* ---- File body replacement (transactional) --------------------- */

/* Replace one file inside the caller's transaction. Keeping this primitive
 * transaction-free lets a sealed complete manifest publish every changed file,
 * every retraction, and its revision bump as one database commit. */
static int ci_replace_file_data_txn(void *conn, int64_t file_id, const char *ext,
                                    const char *content)
{
   char err[CI_ERRBUF] = "";

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
      if (!st)
      {
         LOG_WARN(CI_LOG_TAG, "file content prepare failed: %s", err);
         return -1;
      }
      aimee_pg_bind_int64(st, "?1", file_id);
      aimee_pg_bind_text(st, "?2", content);
      if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_DONE)
      {
         LOG_WARN(CI_LOG_TAG, "file content write failed: %s", err);
         aimee_pg_finalize(st);
         return -1;
      }
      aimee_pg_finalize(st);
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

   return 0;
}

/* Legacy single-file publication. Complete scans use
 * ci_replace_file_data_txn from the session seal below. */
static int ci_replace_file_data(void *conn, const char *project, int64_t file_id, const char *ext,
                                const char *content)
{
   char err[CI_ERRBUF] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
   {
      LOG_WARN(CI_LOG_TAG, "BEGIN failed: %s", err);
      return -1;
   }
   if (ci_replace_file_data_txn(conn, file_id, ext, content) != 0)
   {
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }

   /* Generation-fence check INSIDE the transaction, immediately before
    * COMMIT: an in-flight scan that started pre-fence still cannot commit
    * index rows for a project being purged. The advisory guard serializes
    * this check+commit against the fence-publish transaction, closing the
    * "checked no-fence, fence lands, stale commit" window. */
   if (db2_kb_purge_txn_guard(project) != 0)
   {
      LOG_WARN(CI_LOG_TAG, "purge guard failed for project '%s': aborting index write",
               project ? project : "?");
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }
   if (db2_kb_purge_fence_active(project))
   {
      LOG_WARN(CI_LOG_TAG, "purge fence active for project '%s': aborting index write",
               project ? project : "?");
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }

   if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
   {
      LOG_WARN(CI_LOG_TAG, "COMMIT failed: %s", err);
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }
   return 0;
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

/* Build manifests whose filename legitimately starts with '.' (currently only
 * .gitmodules — git submodule declarations). A thin-client push sends these (the
 * client's code_file_wanted accepts them); the kb must not drop them as "hidden"
 * the way it drops files inside .git/.aimee/etc. dirs (recall §2.2). */
static int ci_is_dotfile_manifest(const char *component, size_t len)
{
   return len == 11 && strncmp(component, ".gitmodules", 11) == 0;
}

/* Like ci_path_has_hidden_component, but a hidden FINAL (filename) component is
 * allowed when it is a wanted dotfile build manifest (ci_is_dotfile_manifest).
 * For thin-client file ingest: the client already gated the file set, and a
 * .gitmodules must be ingested; interior hidden DIRECTORY components (.git/,
 * .github/, ...) are still rejected. The exemption is for the FINAL component at
 * ANY depth (a/b/.gitmodules), not just repo-root — the client gates which paths
 * it sends, so the kb need not second-guess depth. (Leading slashes are tolerated
 * for parity with ci_path_has_hidden_component, though the sole caller already
 * rejects a leading '/'.) */
static int ci_path_ingest_excluded(const char *path)
{
   const char *p = path;
   while (*p == '/')
      p++;

   const char *start = p;
   for (;;)
   {
      if (*p == '/' || *p == '\0')
      {
         size_t len = (size_t)(p - start);
         int is_final = (*p == '\0');
         if (ci_path_component_is_hidden(start, len) &&
             !(is_final && ci_is_dotfile_manifest(start, len)))
            return 1;
         if (is_final)
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

/* --- Git co-change backfill (production path) ---
 *
 * The shipped scanner is canonical_index_scan_project (index.c's index_scan_project
 * is compiled to a stub in aimee-kb), so the git co-change mining and its
 * co_edited-edge read must live here to actually run. Files that change together
 * in a commit are accumulated as co_edited edges (weight = co-change count); the
 * pure pairing/validation policy is shared with index.c via cochange.c. */
#define CI_COCHANGE_MAX_FILES  25  /* bulk-commit gate */
#define CI_COCHANGE_NAME_CAP   64  /* per-commit distinct basenames held */
#define CI_COCHANGE_CKPT_EVERY 200 /* commits between marker checkpoints */

static void ci_cochange_flush(char names[][128], int ncount, cochange_pair_t *pairs, int max_pairs)
{
   int np = cochange_pairs_for_commit(names, ncount, CI_COCHANGE_MAX_FILES, pairs, max_pairs);
   for (int p = 0; p < np; p++)
   {
      int added = 0;
      db2_entity_edge_upsert(pairs[p].a, "co_edited", pairs[p].b, 0, (int)REL_CO_EDITED,
                             (int)NODE_FILE, (int)NODE_FILE, &added);
   }
}

/* Mine git history under `abs_root` into co_edited edges. Incremental via a
 * per-project HEAD marker in kb_runtime_state; the marker is validated as a git
 * object id and shell-escaped before interpolation. See index.c for the full
 * rationale (this is the production twin of index_backfill_cochange). */
static void ci_backfill_cochange(const char *project, const char *abs_root)
{
   char *esc = shell_escape(abs_root);
   if (!esc)
      return;
   char cmd[MAX_PATH_LEN * 2 + 512];
   int rc;

   snprintf(cmd, sizeof(cmd), "git -C %s rev-parse HEAD 2>/dev/null", esc);
   char *head = run_cmd(cmd, &rc);
   if (rc != 0 || !head)
   {
      free(head);
      free(esc);
      return;
   }
   head[strcspn(head, "\r\n")] = '\0';
   if (!cochange_is_hex_sha(head))
   {
      free(head);
      free(esc);
      return;
   }

   char key[192];
   snprintf(key, sizeof(key), "cochange_head:%s", project);
   char marker[128] = "";
   int have_marker = (db2_kb_runtime_state_get(key, marker, sizeof(marker)) == 0 && marker[0]);
   if (have_marker && !cochange_is_hex_sha(marker))
   {
      db2_kb_runtime_state_set(key, head);
      free(head);
      free(esc);
      return;
   }

   char revspec[160] = "";
   if (have_marker)
   {
      if (strcmp(marker, head) == 0)
      {
         free(head);
         free(esc);
         return;
      }
      char *emarker = shell_escape(marker);
      const char *m = emarker ? emarker : marker;
      snprintf(cmd, sizeof(cmd), "git -C %s merge-base --is-ancestor %s HEAD 2>/dev/null", esc, m);
      char *anc = run_cmd(cmd, &rc);
      free(anc);
      if (rc != 0)
      {
         db2_kb_runtime_state_set(key, head);
         free(emarker);
         free(head);
         free(esc);
         return;
      }
      snprintf(revspec, sizeof(revspec), "%s..HEAD", m);
      free(emarker);
   }

   snprintf(cmd, sizeof(cmd),
            "git -C %s log --no-merges -M --reverse --format='@@%%H' --name-only %s 2>/dev/null",
            esc, revspec);
   char *log = run_cmd(cmd, &rc);
   free(esc);
   if (rc != 0 || !log)
   {
      free(log);
      free(head);
      return;
   }

   const int max_pairs = CI_COCHANGE_MAX_FILES * (CI_COCHANGE_MAX_FILES - 1) / 2;
   cochange_pair_t *pairs = malloc(sizeof(*pairs) * (size_t)max_pairs);
   if (!pairs)
   {
      free(log);
      free(head);
      return;
   }

   char names[CI_COCHANGE_NAME_CAP][128];
   int ncount = 0;
   int in_commit = 0;
   char cur_sha[128] = "";
   int done = 0;
   for (char *line = log, *nl; line && *line; line = nl ? nl + 1 : NULL)
   {
      nl = strchr(line, '\n');
      if (nl)
         *nl = '\0';
      if (line[0] == '@' && line[1] == '@')
      {
         if (in_commit)
         {
            ci_cochange_flush(names, ncount, pairs, max_pairs);
            if (cochange_is_hex_sha(cur_sha) && ++done >= CI_COCHANGE_CKPT_EVERY)
            {
               db2_kb_runtime_state_set(key, cur_sha);
               done = 0;
            }
         }
         snprintf(cur_sha, sizeof(cur_sha), "%s", line + 2);
         in_commit = 1;
         ncount = 0;
         continue;
      }
      if (!in_commit || !line[0])
         continue;
      const char *base = strrchr(line, '/');
      base = base ? base + 1 : line;
      const char *ext = ci_get_extension(base);
      if (!ext || !ext[0] || ci_is_binary_extension(ext))
         continue;
      if (ncount >= CI_COCHANGE_NAME_CAP)
         continue;
      snprintf(names[ncount], sizeof(names[0]), "%s", base);
      ncount++;
   }
   if (in_commit)
      ci_cochange_flush(names, ncount, pairs, max_pairs);

   db2_kb_runtime_state_set(key, head);

   free(pairs);
   free(log);
   free(head);
}

/* ---- Complete-manifest scan sessions --------------------------- */

static int ci_scan_id_valid(const char *scan_id)
{
   if (!scan_id || !scan_id[0] || strlen(scan_id) > 96)
      return 0;
   for (const unsigned char *p = (const unsigned char *)scan_id; *p; p++)
      if (!isalnum(*p) && *p != '-' && *p != '_' && *p != '.')
         return 0;
   return 1;
}

static void ci_local_scan_id(char out[97])
{
   static atomic_ullong sequence = 0;
   unsigned long long n = atomic_fetch_add(&sequence, 1) + 1;
   snprintf(out, 97, "local-%llu-%llu", (unsigned long long)time(NULL), n);
}

int canonical_index_scan_begin(const char *name, const char *root_label, const char *scan_id,
                               long long *baseline_revision_out)
{
   if (baseline_revision_out)
      *baseline_revision_out = -1;
   if (!name || !name[0] || !ci_scan_id_valid(scan_id))
      return -1;
   void *conn = ci_conn();
   if (!conn)
      return -1;
   int64_t project_id =
       ci_upsert_project(conn, name, root_label && root_label[0] ? root_label : "remote");
   if (project_id < 0)
      return -1;

   char err[CI_ERRBUF] = "";
   aimee_pg_stmt_t *state = aimee_pg_prepare(
       conn,
       "INSERT INTO code_index_project_state(project_id,revision,sealed_at) VALUES(?1,0,'') "
       "ON CONFLICT(project_id) DO NOTHING",
       err, sizeof(err));
   if (!state)
      return -1;
   aimee_pg_bind_int64(state, "?1", project_id);
   int ok = aimee_pg_step(state, err, sizeof(err)) == AIMEE_PG_DONE;
   aimee_pg_finalize(state);
   if (!ok)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));
   aimee_pg_stmt_t *ins = aimee_pg_prepare(
       conn,
       "INSERT INTO code_scan_sessions(scan_id,project_id,generation,baseline_revision,state,"
       "created_at,sealed_at) SELECT ?1,p.id,p.current_generation,s.revision,'open',?2,'' "
       "FROM projects p JOIN code_index_project_state s ON s.project_id=p.id "
       "WHERE p.id=?3 AND p.lifecycle_state='current' ON CONFLICT(scan_id) DO NOTHING",
       err, sizeof(err));
   if (!ins)
      return -1;
   aimee_pg_bind_text(ins, "?1", scan_id);
   aimee_pg_bind_text(ins, "?2", ts);
   aimee_pg_bind_int64(ins, "?3", project_id);
   ok = aimee_pg_step(ins, err, sizeof(err)) == AIMEE_PG_DONE;
   aimee_pg_finalize(ins);
   if (!ok)
      return -1;

   aimee_pg_stmt_t *read = aimee_pg_prepare(
       conn,
       "SELECT s.baseline_revision FROM code_scan_sessions s JOIN projects p ON p.id=s.project_id "
       "WHERE s.scan_id=?1 AND p.id=?2 AND s.generation=p.current_generation AND s.state='open'",
       err, sizeof(err));
   if (!read)
      return -1;
   aimee_pg_bind_text(read, "?1", scan_id);
   aimee_pg_bind_int64(read, "?2", project_id);
   long long baseline = -1;
   if (aimee_pg_step(read, err, sizeof(err)) == AIMEE_PG_ROW)
      baseline = (long long)aimee_pg_column_int64(read, 0);
   aimee_pg_finalize(read);
   if (baseline < 0)
      return -1;
   if (baseline_revision_out)
      *baseline_revision_out = baseline;
   return 0;
}

int canonical_index_scan_stage(const char *scan_id, const canonical_index_file_input_t *files,
                               int file_count, int *accepted_out)
{
   if (accepted_out)
      *accepted_out = 0;
   if (!ci_scan_id_valid(scan_id) || !files || file_count < 0)
      return -1;
   void *conn = ci_conn();
   if (!conn)
      return -1;
   char err[CI_ERRBUF] = "";
   aimee_pg_stmt_t *open = aimee_pg_prepare(
       conn, "SELECT 1 FROM code_scan_sessions WHERE scan_id=?1 AND state='open'", err,
       sizeof(err));
   if (!open)
      return -1;
   aimee_pg_bind_text(open, "?1", scan_id);
   int exists = aimee_pg_step(open, err, sizeof(err)) == AIMEE_PG_ROW;
   aimee_pg_finalize(open);
   if (!exists)
      return -1;

   aimee_pg_stmt_t *put = aimee_pg_prepare(
       conn,
       "INSERT INTO code_scan_manifest_files(scan_id,path,content,content_hash) "
       "VALUES(?1,?2,?3,?4) ON CONFLICT(scan_id,path) DO UPDATE SET "
       "content=EXCLUDED.content,content_hash=EXCLUDED.content_hash",
       err, sizeof(err));
   if (!put)
      return -1;
   int accepted = 0;
   for (int i = 0; i < file_count; i++)
   {
      const char *path = files[i].rel_path;
      if (!path || !path[0] || path[0] == '/' || ci_path_ingest_excluded(path) ||
          !files[i].content)
         continue;
      char *content = strdup(files[i].content);
      if (!content)
      {
         aimee_pg_finalize(put);
         return -1;
      }
      (void)text_sanitize_utf8(content);
      char hash[32];
      ci_content_hash(content, hash, sizeof(hash));
      aimee_pg_bind_text(put, "?1", scan_id);
      aimee_pg_bind_text(put, "?2", path);
      aimee_pg_bind_text(put, "?3", content);
      aimee_pg_bind_text(put, "?4", hash);
      int rc = aimee_pg_step(put, err, sizeof(err));
      free(content);
      if (rc != AIMEE_PG_DONE)
      {
         aimee_pg_finalize(put);
         return -1;
      }
      aimee_pg_reset(put);
      accepted++;
   }
   aimee_pg_finalize(put);
   if (accepted_out)
      *accepted_out = accepted;
   return 0;
}

static int ci_scan_session_count(void *conn, const char *scan_id)
{
   char err[CI_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT COUNT(*) FROM code_scan_manifest_files WHERE scan_id=?1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", scan_id);
   int count = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      count = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return count;
}

int canonical_index_scan_seal(const char *scan_id, int expected_files,
                              canonical_index_seal_result_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!ci_scan_id_valid(scan_id) || expected_files < 0)
      return -1;
   void *conn = ci_conn();
   if (!conn)
      return -1;
   char err[CI_ERRBUF] = "";
   char project[256] = "";
   aimee_pg_stmt_t *project_q = aimee_pg_prepare(
       conn,
       "SELECT p.name FROM code_scan_sessions s JOIN projects p ON p.id=s.project_id "
       "WHERE s.scan_id=?1",
       err, sizeof(err));
   if (!project_q)
      return -1;
   aimee_pg_bind_text(project_q, "?1", scan_id);
   if (aimee_pg_step(project_q, err, sizeof(err)) == AIMEE_PG_ROW)
      snprintf(project, sizeof(project), "%s", aimee_pg_column_text(project_q, 0));
   aimee_pg_finalize(project_q);
   if (!project[0])
      return -2;
   if (aimee_pg_exec(conn, aimee_pg_is_shim() ? "BEGIN" : "BEGIN ISOLATION LEVEL SERIALIZABLE",
                     err, sizeof(err)) != 0)
      return -1;
   /* Purge and scan publication share the project advisory lock. Take it
    * before row locks on both paths to avoid lock-order inversions. */
   if (db2_kb_purge_txn_guard(project) != 0 || db2_kb_purge_fence_active(project))
      goto storage_fail;

   const char *session_sql = aimee_pg_is_shim()
                                 ? "SELECT s.project_id,s.generation,s.baseline_revision,s.state,"
                                   "p.name,p.current_generation,ps.revision FROM code_scan_sessions s "
                                   "JOIN projects p ON p.id=s.project_id JOIN code_index_project_state ps "
                                   "ON ps.project_id=p.id WHERE s.scan_id=?1"
                                 : "SELECT s.project_id,s.generation,s.baseline_revision,s.state,"
                                   "p.name,p.current_generation,ps.revision FROM code_scan_sessions s "
                                   "JOIN projects p ON p.id=s.project_id JOIN code_index_project_state ps "
                                   "ON ps.project_id=p.id WHERE s.scan_id=?1 FOR UPDATE OF s,ps";
   aimee_pg_stmt_t *session = aimee_pg_prepare(conn, session_sql, err, sizeof(err));
   if (!session)
      goto storage_fail;
   aimee_pg_bind_text(session, "?1", scan_id);
   if (aimee_pg_step(session, err, sizeof(err)) != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(session);
      goto stale;
   }
   int64_t project_id = aimee_pg_column_int64(session, 0);
   int64_t generation = aimee_pg_column_int64(session, 1);
   int64_t baseline = aimee_pg_column_int64(session, 2);
   char state[16];
   snprintf(state, sizeof(state), "%s", aimee_pg_column_text(session, 3));
   int64_t current_generation = aimee_pg_column_int64(session, 5);
   int64_t current_revision = aimee_pg_column_int64(session, 6);
   aimee_pg_finalize(session);
   if (strcmp(state, "open") != 0 || generation != current_generation ||
       baseline != current_revision || ci_scan_session_count(conn, scan_id) != expected_files)
      goto stale;

   char ts[32];
   now_utc(ts, sizeof(ts));
   aimee_pg_stmt_t *rows = aimee_pg_prepare(
       conn,
       "SELECT path,content,content_hash FROM code_scan_manifest_files WHERE scan_id=?1 "
       "ORDER BY path",
       err, sizeof(err));
   if (!rows)
      goto storage_fail;
   aimee_pg_bind_text(rows, "?1", scan_id);
   int indexed = 0;
   int row_rc = AIMEE_PG_DONE;
   while ((row_rc = aimee_pg_step(rows, err, sizeof(err))) == AIMEE_PG_ROW)
   {
      char *path = strdup(aimee_pg_column_text(rows, 0));
      char *content = strdup(aimee_pg_column_text(rows, 1));
      char hash[32];
      snprintf(hash, sizeof(hash), "%s", aimee_pg_column_text(rows, 2));
      if (!path || !content)
      {
         free(path);
         free(content);
         aimee_pg_finalize(rows);
         goto storage_fail;
      }
      int changed = 1;
      aimee_pg_stmt_t *old = aimee_pg_prepare(
          conn,
          "SELECT f.hash FROM files f JOIN projects p ON p.id=f.project_id WHERE f.project_id=?1 "
          "AND f.generation=p.current_generation AND f.path=?2",
          err, sizeof(err));
      if (!old)
      {
         free(path);
         free(content);
         aimee_pg_finalize(rows);
         goto storage_fail;
      }
      aimee_pg_bind_int64(old, "?1", project_id);
      aimee_pg_bind_text(old, "?2", path);
      if (aimee_pg_step(old, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         const char *old_hash = aimee_pg_column_text(old, 0);
         changed = !old_hash || strcmp(old_hash, hash) != 0;
      }
      aimee_pg_finalize(old);
      int64_t file_id = ci_upsert_file(conn, project_id, path, ts);
      if (file_id < 0 ||
          (changed && ci_replace_file_data_txn(conn, file_id, ci_get_extension(path), content) != 0))
      {
         free(path);
         free(content);
         aimee_pg_finalize(rows);
         goto storage_fail;
      }
      indexed += changed;
      free(path);
      free(content);
   }
   aimee_pg_finalize(rows);
   if (row_rc != AIMEE_PG_DONE)
      goto storage_fail;

   aimee_pg_stmt_t *del = aimee_pg_prepare(
       conn,
       "DELETE FROM files WHERE project_id=?1 AND generation=?2 AND NOT EXISTS "
       "(SELECT 1 FROM code_scan_manifest_files m WHERE m.scan_id=?3 AND m.path=files.path)",
       err, sizeof(err));
   if (!del)
      goto storage_fail;
   aimee_pg_bind_int64(del, "?1", project_id);
   aimee_pg_bind_int64(del, "?2", generation);
   aimee_pg_bind_text(del, "?3", scan_id);
   if (aimee_pg_step(del, err, sizeof(err)) != AIMEE_PG_DONE)
   {
      aimee_pg_finalize(del);
      goto storage_fail;
   }
   int retracted = aimee_pg_stmt_changes(del);
   aimee_pg_finalize(del);

   aimee_pg_stmt_t *bump = aimee_pg_prepare(
       conn,
       "UPDATE code_index_project_state SET revision=revision+1,sealed_at=?1 WHERE project_id=?2 "
       "RETURNING revision",
       err, sizeof(err));
   if (!bump)
      goto storage_fail;
   aimee_pg_bind_text(bump, "?1", ts);
   aimee_pg_bind_int64(bump, "?2", project_id);
   long long revision = -1;
   if (aimee_pg_step(bump, err, sizeof(err)) == AIMEE_PG_ROW)
      revision = (long long)aimee_pg_column_int64(bump, 0);
   aimee_pg_finalize(bump);
   if (revision < 0)
      goto storage_fail;

   aimee_pg_stmt_t *done = aimee_pg_prepare(
       conn, "UPDATE code_scan_sessions SET state='sealed',sealed_at=?1 WHERE scan_id=?2", err,
       sizeof(err));
   if (!done)
      goto storage_fail;
   aimee_pg_bind_text(done, "?1", ts);
   aimee_pg_bind_text(done, "?2", scan_id);
   int done_ok = aimee_pg_step(done, err, sizeof(err)) == AIMEE_PG_DONE;
   aimee_pg_finalize(done);
   if (!done_ok)
      goto storage_fail;
   if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
   {
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      LOG_WARN(CI_LOG_TAG, "scan seal commit failed for '%s': %s", scan_id, err);
      return -1;
   }
   if (out)
   {
      out->revision = revision;
      out->files_indexed = indexed;
      out->files_retracted = retracted;
   }
   /* Optional CSS projections own their own transactions, so converge them
    * only after the source snapshot is visible. */
   if (config_present() && config_css_style_graph_enabled())
   {
      for (int offset = 0;; offset++)
      {
         aimee_pg_stmt_t *css = aimee_pg_prepare(
             conn,
             "SELECT path,content FROM code_scan_manifest_files WHERE scan_id=?1 ORDER BY path "
             "LIMIT 1 OFFSET ?2",
             err, sizeof(err));
         if (!css)
            break;
         aimee_pg_bind_text(css, "?1", scan_id);
         aimee_pg_bind_int(css, "?2", offset);
         if (aimee_pg_step(css, err, sizeof(err)) != AIMEE_PG_ROW)
         {
            aimee_pg_finalize(css);
            break;
         }
         char *path = strdup(aimee_pg_column_text(css, 0));
         char *content = strdup(aimee_pg_column_text(css, 1));
         aimee_pg_finalize(css);
         if (!path || !content)
         {
            free(path);
            free(content);
            break;
         }
         int64_t file_id = ci_resolve_file_id(conn, project_id, path);
         if (file_id >= 0)
            ci_css_index_file(file_id, ci_get_extension(path), content, 1);
         free(path);
         free(content);
      }
   }
   return 0;

stale:
   aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
   return -2;
storage_fail:
   aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
   LOG_WARN(CI_LOG_TAG, "scan seal failed for '%s': %s", scan_id, err);
   return -1;
}

int canonical_index_scan_abort(const char *scan_id)
{
   if (!ci_scan_id_valid(scan_id))
      return -1;
   void *conn = ci_conn();
   if (!conn)
      return -1;
   char err[CI_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "UPDATE code_scan_sessions SET state='aborted' WHERE scan_id=?1 AND state='open'", err,
       sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", scan_id);
   int rc = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

static int ci_path_ptr_cmp(const void *a, const void *b)
{
   return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void ci_verify_example(canonical_index_verify_result_t *out, const char *kind,
                              const char *path)
{
   if (out->example_count >= CANONICAL_INDEX_VERIFY_EXAMPLES)
      return;
   snprintf(out->examples[out->example_count], sizeof(out->examples[0]), "%s:%s", kind, path);
   out->example_count++;
}

int canonical_index_verify_project(const char *name, const char *root, int deep,
                                   canonical_index_verify_result_t *out)
{
   if (!out || !name || !name[0] || !root || !root[0])
      return -1;
   memset(out, 0, sizeof(*out));
   char abs_root[MAX_PATH_LEN];
   if (!realpath(root, abs_root))
   {
      out->unavailable = 1;
      return 0;
   }
   void *conn = ci_conn();
   if (!conn)
      return -1;
   int64_t project_id = ci_resolve_project_id(conn, name);
   if (project_id < 0)
      return -1;
   char err[CI_ERRBUF] = "";
   aimee_pg_stmt_t *meta = aimee_pg_prepare(
       conn,
       "SELECT COUNT(f.id),COALESCE(s.revision,0) FROM projects p "
       "LEFT JOIN files f ON f.project_id=p.id AND f.generation=p.current_generation "
       "LEFT JOIN code_index_project_state s ON s.project_id=p.id WHERE p.id=?1 "
       "GROUP BY s.revision",
       err, sizeof(err));
   if (!meta)
      return -1;
   aimee_pg_bind_int64(meta, "?1", project_id);
   if (aimee_pg_step(meta, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      out->indexed_files = aimee_pg_column_int(meta, 0);
      out->index_revision = (long long)aimee_pg_column_int64(meta, 1);
   }
   aimee_pg_finalize(meta);

   ci_file_list_t list = {0};
   ci_collect_text_files(abs_root, &list);
   if (list.count > 1)
      qsort(list.paths, (size_t)list.count, sizeof(list.paths[0]), ci_path_ptr_cmp);
   out->workspace_files = list.count;
   for (int i = 0; i < list.count; i++)
   {
      const char *rel = list.paths[i] + strlen(abs_root);
      if (*rel == '/')
         rel++;
      aimee_pg_stmt_t *hash_q = aimee_pg_prepare(
          conn,
          "SELECT f.hash FROM files f JOIN projects p ON p.id=f.project_id WHERE f.project_id=?1 "
          "AND f.generation=p.current_generation AND f.path=?2",
          err, sizeof(err));
      if (!hash_q)
      {
         for (; i < list.count; i++)
            free(list.paths[i]);
         free(list.paths);
         return -1;
      }
      aimee_pg_bind_int64(hash_q, "?1", project_id);
      aimee_pg_bind_text(hash_q, "?2", rel);
      int row = aimee_pg_step(hash_q, err, sizeof(err));
      char stored_hash[32] = "";
      if (row == AIMEE_PG_ROW)
         snprintf(stored_hash, sizeof(stored_hash), "%s", aimee_pg_column_text(hash_q, 0));
      aimee_pg_finalize(hash_q);
      if (row != AIMEE_PG_ROW)
      {
         out->unindexed_files++;
         ci_verify_example(out, "unindexed", rel);
      }
      else if (deep)
      {
         size_t content_len = 0;
         char *content = ci_read_file_content(list.paths[i], &content_len);
         if (!content)
         {
            for (; i < list.count; i++)
               free(list.paths[i]);
            free(list.paths);
            out->unavailable = 1;
            return 0;
         }
         (void)text_sanitize_utf8(content);
         char actual_hash[32];
         ci_content_hash(content, actual_hash, sizeof(actual_hash));
         free(content);
         if (strcmp(stored_hash, actual_hash) != 0)
         {
            out->modified_files++;
            ci_verify_example(out, "modified", rel);
         }
      }
      free(list.paths[i]);
   }
   free(list.paths);

   aimee_pg_stmt_t *paths = aimee_pg_prepare(
       conn,
       "SELECT f.path FROM files f JOIN projects p ON p.id=f.project_id WHERE f.project_id=?1 "
       "AND f.generation=p.current_generation ORDER BY f.path",
       err, sizeof(err));
   if (!paths)
      return -1;
   aimee_pg_bind_int64(paths, "?1", project_id);
   int row_rc;
   while ((row_rc = aimee_pg_step(paths, err, sizeof(err))) == AIMEE_PG_ROW)
   {
      const char *rel = aimee_pg_column_text(paths, 0);
      char full[MAX_PATH_LEN];
      snprintf(full, sizeof(full), "%s/%s", abs_root, rel ? rel : "");
      struct stat st;
      if (stat(full, &st) != 0)
      {
         out->missing_files++;
         ci_verify_example(out, "missing", rel ? rel : "");
      }
   }
   aimee_pg_finalize(paths);
   return row_rc == AIMEE_PG_DONE ? 0 : -1;
}

int canonical_index_scan_project(const char *name, const char *root, int force, int *inspected_out)
{
   (void)force; /* content hashes, not mtimes, decide whether publication work is needed */
   if (inspected_out)
      *inspected_out = 0;

   void *conn = ci_conn();
   if (!conn)
      return -1;

   /* Git co-change remains a convergent projection after the source seal. */
   int cfg_ok = (config_present());
   int cochange_on = cfg_ok && config_code_cochange_git_enabled();

   char abs_root[MAX_PATH_LEN];
   if (realpath(root, abs_root) == NULL)
      snprintf(abs_root, sizeof(abs_root), "%s", root);

   if (ci_path_has_hidden_component(abs_root))
   {
      fprintf(stderr, "aimee index: refusing to scan excluded root: %s\n", abs_root);
      return -1;
   }

   char scan_id[97];
   ci_local_scan_id(scan_id);
   if (canonical_index_scan_begin(name, abs_root, scan_id, NULL) != 0)
      return -1;

   ci_file_list_t list = {0};
   ci_collect_text_files(abs_root, &list);

   if (inspected_out)
      *inspected_out = list.count;

   int accepted_total = 0;
   for (int i = 0; i < list.count; i++)
   {
      const char *full = list.paths[i];
      const char *rel = full + strlen(abs_root);
      if (*rel == '/')
         rel++;

      size_t content_len;
      char *content = ci_read_file_content(full, &content_len);
      if (!content)
      {
         for (int j = i; j < list.count; j++)
            free(list.paths[j]);
         free(list.paths);
         canonical_index_scan_abort(scan_id);
         return -1;
      }
      canonical_index_file_input_t input = {.rel_path = rel, .content = content};
      int accepted = 0;
      if (canonical_index_scan_stage(scan_id, &input, 1, &accepted) != 0 || accepted != 1)
      {
         free(content);
         for (int j = i; j < list.count; j++)
            free(list.paths[j]);
         free(list.paths);
         canonical_index_scan_abort(scan_id);
         return -1;
      }
      accepted_total += accepted;
      free(content);
      free(list.paths[i]);
   }

   free(list.paths);

   canonical_index_seal_result_t sealed;
   if (canonical_index_scan_seal(scan_id, accepted_total, &sealed) != 0)
   {
      canonical_index_scan_abort(scan_id);
      return -1;
   }

   /* Seed co_edited edges from git history (incremental after the first scan). */
   if (cochange_on)
      ci_backfill_cochange(name, abs_root);

   return sealed.files_indexed;
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

   /* CSS style-graph and git co-change write paths are opt-in; read once per scan. */
   int cfg_ok = (config_present());
   int css_on = cfg_ok && config_css_style_graph_enabled();
   int cochange_on = cfg_ok && config_code_cochange_git_enabled();

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
      if (!rel || !rel[0] || rel[0] == '/' || ci_path_ingest_excluded(rel) || !content)
         continue;

      /* Remote/durable input is caller-owned. Keep it immutable while enforcing
       * the same Postgres UTF-8 boundary as the filesystem scanner. */
      char *clean_content = strdup(content);
      if (!clean_content)
         return -1;
      (void)text_sanitize_utf8(clean_content);

      inspected++;
      int64_t file_id = ci_upsert_file(conn, project_id, rel, ts);
      if (file_id < 0)
      {
         free(clean_content);
         continue;
      }

      const char *css_ext = ci_get_extension(rel);
      if (ci_replace_file_data(conn, name, file_id, css_ext, clean_content) != 0)
      {
         /* Purge fence: abort the whole scan for this project. */
         free(clean_content);
         if (inspected_out)
            *inspected_out = inspected;
         return -1;
      }
      ci_css_index_file(file_id, css_ext, clean_content, css_on);
      free(clean_content);
      scanned++;
   }

   if (inspected_out)
      *inspected_out = inspected;

   /* Seed co_edited edges from git history when the pushed root_label is a real
    * repo the kb can reach (co-located client). A "remote" / inaccessible path
    * fails the git probe and no-ops, so this is safe for true remote pushes. */
   if (cochange_on && root_label && root_label[0] && strcmp(root_label, "remote") != 0)
      ci_backfill_cochange(name, root_label);

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
                                          " WHERE lifecycle_state = 'current'"
                                          " AND root NOT LIKE '%/.%'"
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
                                 "   AND p.lifecycle_state = 'current'"
                                 "   AND f.generation = p.current_generation"
                                 "   AND (?3 = '' OR p.name = ?3)"
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
                                      "   AND p.lifecycle_state = 'current'"
                                      "   AND f.generation = p.current_generation"
                                      "   AND (?3 = '' OR p.name = ?3)"
                                      "   AND f.path NOT LIKE '.%'"
                                      "   AND f.path NOT LIKE '%/.%'"
                                      "   AND p.root NOT LIKE '%/.%'"
                                      " GROUP BY p.name, f.path, t.line, t.kind"
                                      " ORDER BY CASE WHEN t.kind = 'definition' THEN 0 ELSE 1 END,"
                                      " p.name, f.path"
                                      " LIMIT ?2";

static const char *ci_find_excluding_sql =
    "SELECT p.name, f.path, t.line, t.kind"
    " FROM terms t"
    " JOIN files f ON f.id = t.file_id"
    " JOIN projects p ON p.id = f.project_id"
    " WHERE t.name = ?1"
    "   AND p.lifecycle_state = 'current'"
    "   AND f.generation = p.current_generation"
    "   AND p.name <> ?3"
    "   AND f.path NOT LIKE '.%'"
    "   AND f.path NOT LIKE '%/.%'"
    "   AND p.root NOT LIKE '%/.%'"
    " GROUP BY p.name, f.path, t.line, t.kind"
    " ORDER BY CASE WHEN t.kind = 'definition' THEN 0 ELSE 1 END,"
    " p.name, f.path"
    " LIMIT ?2";

static const char *ci_find_like_excluding_sql =
    "SELECT p.name, f.path, t.line, t.kind"
    " FROM terms t"
    " JOIN files f ON f.id = t.file_id"
    " JOIN projects p ON p.id = f.project_id"
    " WHERE t.name LIKE ?1 ESCAPE '\\'"
    "   AND p.lifecycle_state = 'current'"
    "   AND f.generation = p.current_generation"
    "   AND p.name <> ?3"
    "   AND f.path NOT LIKE '.%'"
    "   AND f.path NOT LIKE '%/.%'"
    "   AND p.root NOT LIKE '%/.%'"
    " GROUP BY p.name, f.path, t.line, t.kind"
    " ORDER BY CASE WHEN t.kind = 'definition' THEN 0 ELSE 1 END, p.name, f.path"
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

static int ci_find_scoped(const char *project, const char *excluded_project, const char *identifier,
                          term_hit_t *out, int max)
{
   if (project && project[0] && excluded_project && excluded_project[0])
      return -1;
   void *conn = ci_conn();
   if (!conn)
      return -1;

   char err[CI_ERRBUF] = "";
   const int excluding = excluded_project && excluded_project[0];
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, excluding ? ci_find_excluding_sql : ci_find_sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", identifier);
   aimee_pg_bind_int(st, "?2", max);
   aimee_pg_bind_text(st, "?3",
                      excluding ? excluded_project : (project && project[0] ? project : ""));
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
      aimee_pg_stmt_t *st2 = aimee_pg_prepare(
          conn, excluding ? ci_find_like_excluding_sql : ci_find_like_sql, err, sizeof(err));
      if (!st2)
         break;
      aimee_pg_bind_text(st2, "?1", pattern);
      aimee_pg_bind_int(st2, "?2", max);
      aimee_pg_bind_text(st2, "?3",
                         excluding ? excluded_project : (project && project[0] ? project : ""));
      count = ci_drain_term_hits(st2, out, max, err, sizeof(err));
      aimee_pg_finalize(st2);
   }
   return count;
}

int canonical_index_find_project(const char *project, const char *identifier, term_hit_t *out,
                                 int max)
{
   return ci_find_scoped(project, NULL, identifier, out, max);
}

int canonical_index_find_excluding_project(const char *excluded_project, const char *identifier,
                                           term_hit_t *out, int max)
{
   if (!excluded_project || !excluded_project[0])
      return -1;
   return ci_find_scoped(NULL, excluded_project, identifier, out, max);
}

int canonical_index_find(const char *identifier, term_hit_t *out, int max)
{
   return canonical_index_find_project(NULL, identifier, out, max);
}

int canonical_index_blast_radius(const char *project, const char *file_path, blast_radius_t *out)
{
   if (!project || !file_path || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   snprintf(out->file, sizeof(out->file), "%s", file_path);
   if (db2_code_index_blast_radius(project, file_path, out) != 0)
      return -1;

   /* Expand with co_edited graph edges (weight > 3): files that historically
    * change together but have no structural import edge. Basename-keyed legacy
    * projections are admitted only when they resolve to one current file. */
   {
      const char *slash = strrchr(file_path, '/');
      const char *match_name = slash ? slash + 1 : file_path;

      char co_buf[16][128];
      int n = db2_entity_edge_co_targets(match_name, "co_edited", 3, co_buf, 16);
      for (int b = 0; b < n && out->dependent_count < CI_MAX_DEPS; b++)
      {
         const char *related = co_buf[b];
         if (!related[0] || strcmp(related, match_name) == 0)
            continue;
         char resolved[MAX_PATH_LEN];
         if (db2_code_index_unique_file_basename(project, related, resolved, sizeof(resolved)) !=
                 1 ||
             strcmp(resolved, file_path) == 0)
            continue;
         int found = -1;
         for (int d = 0; d < out->dependent_count; d++)
         {
            if (strcmp(out->dependents[d], resolved) == 0 &&
                strcmp(out->dependent_meta[d].project, project) == 0)
            {
               found = d;
               break;
            }
         }
         if (found < 0)
         {
            if (out->dependent_count >= CI_MAX_DEPS)
               continue;
            found = out->dependent_count++;
            snprintf(out->dependents[found], MAX_PATH_LEN, "%s", resolved);
            snprintf(out->dependent_meta[found].project, sizeof(out->dependent_meta[found].project),
                     "%s", project);
            out->dependent_meta[found].generation = out->generation;
            snprintf(out->dependent_meta[found].freshness,
                     sizeof(out->dependent_meta[found].freshness), "current");
            snprintf(out->dependent_meta[found].confidence,
                     sizeof(out->dependent_meta[found].confidence), "low");
         }
         char *provenance = out->dependent_meta[found].provenance;
         if (provenance[0] && !strstr(provenance, "projection"))
            strncat(provenance, ",projection",
                    sizeof(out->dependent_meta[found].provenance) - strlen(provenance) - 1);
         else if (!provenance[0])
            snprintf(provenance, sizeof(out->dependent_meta[found].provenance), "projection");
      }
   }

   db2_code_index_blast_radius_local_first(project, out);

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

static int ci_find_callers_scoped(const char *project, const char *excluded_project,
                                  const char *symbol, caller_hit_t *out, int max)
{
   if (project && project[0] && excluded_project && excluded_project[0])
      return -1;
   void *conn = ci_conn();
   if (!conn)
      return -1;

   char err[CI_ERRBUF] = "";
   const char *sql_all = "SELECT p.name, f.path, cc.caller, cc.line"
                         " FROM code_calls cc"
                         " JOIN files f ON f.id = cc.file_id"
                         " JOIN projects p ON p.id = f.project_id"
                         " WHERE cc.callee = ?1"
                         "   AND p.lifecycle_state = 'current'"
                         "   AND f.generation = p.current_generation"
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
                          "   AND p.lifecycle_state = 'current'"
                          "   AND f.generation = p.current_generation"
                          "   AND f.path NOT LIKE '.%'"
                          "   AND f.path NOT LIKE '%/.%'"
                          "   AND p.root NOT LIKE '%/.%'"
                          " ORDER BY f.path, cc.line"
                          " LIMIT ?3";
   const char *sql_excluding = "SELECT p.name, f.path, cc.caller, cc.line"
                               " FROM code_calls cc"
                               " JOIN files f ON f.id = cc.file_id"
                               " JOIN projects p ON p.id = f.project_id"
                               " WHERE cc.callee = ?1 AND p.name <> ?2"
                               "   AND p.lifecycle_state = 'current'"
                               "   AND f.generation = p.current_generation"
                               "   AND f.path NOT LIKE '.%'"
                               "   AND f.path NOT LIKE '%/.%'"
                               "   AND p.root NOT LIKE '%/.%'"
                               " ORDER BY p.name, f.path, cc.line"
                               " LIMIT ?3";

   aimee_pg_stmt_t *st;
   if (excluded_project && excluded_project[0])
   {
      st = aimee_pg_prepare(conn, sql_excluding, err, sizeof(err));
      if (!st)
         return 0;
      aimee_pg_bind_text(st, "?1", symbol);
      aimee_pg_bind_text(st, "?2", excluded_project);
      aimee_pg_bind_int(st, "?3", max);
   }
   else if (project && project[0])
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

int canonical_index_find_callers(const char *project, const char *symbol, caller_hit_t *out,
                                 int max)
{
   return ci_find_callers_scoped(project, NULL, symbol, out, max);
}

int canonical_index_find_callers_excluding_project(const char *excluded_project, const char *symbol,
                                                   caller_hit_t *out, int max)
{
   if (!excluded_project || !excluded_project[0])
      return -1;
   return ci_find_callers_scoped(NULL, excluded_project, symbol, out, max);
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
                            "    AND p.lifecycle_state = 'current'"
                            "    AND f.generation = p.current_generation"
                            "    AND f.path NOT LIKE '.%'"
                            "    AND f.path NOT LIKE '%/.%'"
                            "    AND p.root NOT LIKE '%/.%'),"
                            " (SELECT COUNT(*) FROM terms t"
                            "  JOIN files f ON f.id = t.file_id"
                            "  JOIN projects p ON p.id = f.project_id"
                            "  WHERE p.name = ?2 AND t.kind = 'definition'"
                            "    AND p.lifecycle_state = 'current'"
                            "    AND f.generation = p.current_generation"
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
       "   AND p.lifecycle_state = 'current'"
       "   AND f.generation = p.current_generation"
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

int canonical_index_code_search_excluding_project(const char *query, const char *excluded_project,
                                                  code_search_hit_t *out, int max, int enrich)
{
   return db2_code_index_code_search_excluding_project(query, excluded_project, out, max, enrich);
}
