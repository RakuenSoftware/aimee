#include "aimee.h"
#include "agent_tools.h"
#include "agent_source_authority.h"
#include "dstr.h"
#include "kb_client.h"
#include "platform_process.h"
#include "util.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/* Per-delegate source-authority context.
 *
 * This was previously read from process-global env vars (AIMEE_DELEGATE_*),
 * which raced once delegates began running concurrently on detached threads
 * (on-demand execution): a delegate's code_search overlay would resolve its
 * staleness/freshness checks against ANOTHER concurrently-running delegate's
 * worktree root, producing wrong annotations that varied with thread timing.
 *
 * The authoritative source for an in-process delegate run is now thread-local,
 * installed by delegate_run_ctx_enter (see server_compute.c). The env vars
 * remain the fallback for cross-process child clients (cmd_index, the
 * aimee-client shell-out), which inherit them and have their own single-
 * threaded environment. tl_sa_active distinguishes "this thread is running a
 * delegate" (read TLS) from "not in a delegate" (read env / no-op). */
static __thread int tl_sa_active = 0;
static __thread int tl_sa_authority = 0;
static __thread char tl_sa_root[MAX_PATH_LEN] = {0};
static __thread char *tl_sa_paths = NULL; /* heap, newline-joined; may be NULL */

void agent_source_authority_tls_set(int authority, const char *worktree_root, const char *paths)
{
   tl_sa_active = 1;
   tl_sa_authority = authority ? 1 : 0;
   snprintf(tl_sa_root, sizeof(tl_sa_root), "%s", worktree_root ? worktree_root : "");
   free(tl_sa_paths);
   tl_sa_paths = (paths && paths[0]) ? strdup(paths) : NULL;
}

void agent_source_authority_tls_capture(agent_source_authority_snapshot_t *snap)
{
   if (!snap)
      return;
   snap->active = tl_sa_active;
   snap->authority = tl_sa_authority;
   snprintf(snap->root, sizeof(snap->root), "%s", tl_sa_root);
   snap->paths = tl_sa_paths ? strdup(tl_sa_paths) : NULL;
}

void agent_source_authority_tls_restore(agent_source_authority_snapshot_t *snap)
{
   if (!snap)
      return;
   tl_sa_active = snap->active;
   tl_sa_authority = snap->authority;
   snprintf(tl_sa_root, sizeof(tl_sa_root), "%s", snap->root);
   free(tl_sa_paths);
   tl_sa_paths = snap->paths; /* transfer ownership */
   snap->paths = NULL;
}

/* Mirror this thread's source-authority context into the process env so a
 * fork()ed child that re-execs (cmd_index, the aimee-client shell-out) inherits
 * it. Call ONLY in the post-fork/pre-exec child, where the single-threaded child
 * is immune to the cross-thread env clobber that affects the parent. No-op when
 * not inside a delegate run (the inherited env is left untouched). */
void agent_source_authority_export_env(void)
{
   if (!tl_sa_active)
      return;
   platform_setenv("AIMEE_DELEGATE_SOURCE_AUTHORITY", tl_sa_authority ? "1" : "");
   platform_setenv("AIMEE_DELEGATE_WORKTREE_ROOT", tl_sa_root);
   platform_setenv("AIMEE_DELEGATE_SOURCE_PATHS", tl_sa_paths ? tl_sa_paths : "");
}

static int source_authority_enabled(void)
{
   if (tl_sa_active)
      return tl_sa_authority;
   const char *enabled = getenv("AIMEE_DELEGATE_SOURCE_AUTHORITY");
   return enabled && enabled[0] && strcmp(enabled, "0") != 0;
}

static const char *source_authority_root(void)
{
   if (tl_sa_active)
      return tl_sa_root[0] ? tl_sa_root : NULL;
   const char *root = getenv("AIMEE_DELEGATE_WORKTREE_ROOT");
   return root && root[0] ? root : NULL;
}

/* Newline-joined source paths for the current delegate (TLS), or the env
 * fallback for cross-process clients. May return NULL. */
static const char *source_authority_paths(void)
{
   if (tl_sa_active)
      return tl_sa_paths;
   return getenv("AIMEE_DELEGATE_SOURCE_PATHS");
}

static void source_authority_resolve_path(const char *path, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (!path || !path[0])
      return;
   if (path[0] == '/')
   {
      snprintf(out, out_len, "%s", path);
      return;
   }
   const char *root = source_authority_root();
   if (root && root[0])
      snprintf(out, out_len, "%s/%s", root, path);
   else
   {
      char cwd[MAX_PATH_LEN];
      if (getcwd(cwd, sizeof(cwd)))
         snprintf(out, out_len, "%s/%s", cwd, path);
      else
         snprintf(out, out_len, "%s", path);
   }
}

static int source_path_matches_hit(const char *source_path, const char *hit_path)
{
   if (!source_path || !source_path[0] || !hit_path || !hit_path[0])
      return 0;
   if (strcmp(source_path, hit_path) == 0)
      return 1;
   const char *source_base = strrchr(source_path, '/');
   const char *hit_base = strrchr(hit_path, '/');
   source_base = source_base ? source_base + 1 : source_path;
   hit_base = hit_base ? hit_base + 1 : hit_path;
   return source_base[0] && strcmp(source_base, hit_base) == 0;
}

static int source_paths_contains(const char *hit_path)
{
   const char *paths = source_authority_paths();
   if (!paths || !paths[0] || !hit_path || !hit_path[0])
      return 0;

   const char *p = paths;
   while (*p)
   {
      const char *nl = strchr(p, '\n');
      size_t len = nl ? (size_t)(nl - p) : strlen(p);
      if (len > 0)
      {
         char one[MAX_PATH_LEN];
         size_t copy = len < sizeof(one) - 1 ? len : sizeof(one) - 1;
         memcpy(one, p, copy);
         one[copy] = '\0';
         if (source_path_matches_hit(one, hit_path))
            return 1;
      }
      if (!nl)
         break;
      p = nl + 1;
   }
   return 0;
}

static int line_matches_query(const char *line, const char *query)
{
   if (str_contains_ci(line, query))
      return 1;

   char qcopy[256];
   snprintf(qcopy, sizeof(qcopy), "%s", query ? query : "");
   char *saveptr = NULL;
   char *tok = strtok_r(qcopy, " \t\r\n,.;:!?()[]{}\"'", &saveptr);
   while (tok)
   {
      if (strlen(tok) > 2 && str_contains_ci(line, tok))
         return 1;
      tok = strtok_r(NULL, " \t\r\n,.;:!?()[]{}\"'", &saveptr);
   }
   return 0;
}

static int git_path_differs_from_main(const char *root, const char *file_path)
{
   if (!root || !root[0] || !file_path || !file_path[0] || file_path[0] == '/')
      return -1;

   const char *argv[] = {"git", "-C", root, "diff", "--quiet", "main", "--", file_path, NULL};
   char *out = NULL;
   int rc = safe_exec_capture(argv, &out, 4096);
   free(out);
   if (rc == 0)
      return 0;
   if (rc == 1)
      return 1;
   return -1;
}

static void current_hit_path(const char *file_path, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (!file_path || !file_path[0])
      return;
   if (file_path[0] == '/')
      snprintf(out, out_len, "%s", file_path);
   else
      source_authority_resolve_path(file_path, out, out_len);
}

void agent_source_add_index_freshness(cJSON *obj, const char *project, const char *file_path)
{
   (void)project;
   if (!obj)
      return;
   cJSON_AddStringToObject(obj, "evidence", "canonical_index");
   cJSON_AddStringToObject(obj, "authority", "discovery_only");
   cJSON_AddStringToObject(obj, "authority_note",
                           "Use index/search as discovery. For file contents, trust source "
                           "packets and read_file from the delegate worktree over indexed "
                           "snippets when they differ.");

   if (source_authority_enabled() && source_paths_contains(file_path))
   {
      cJSON_AddStringToObject(obj, "freshness", "source_packet_current");
      cJSON_ReplaceItemInObject(obj, "authority", cJSON_CreateString("current_source"));
      return;
   }

   char current[MAX_PATH_LEN];
   current_hit_path(file_path, current, sizeof(current));
   struct stat st;
   if (!current[0] || stat(current, &st) != 0 || !S_ISREG(st.st_mode))
   {
      cJSON_AddStringToObject(obj, "freshness", "canonical_index_unverified");
      return;
   }
   cJSON_AddStringToObject(obj, "current_file", current);

   const char *root = source_authority_root();
   if (root && file_path && file_path[0] && file_path[0] != '/')
   {
      int differs = git_path_differs_from_main(root, file_path);
      if (differs > 0)
      {
         cJSON_AddStringToObject(obj, "freshness", "worktree_differs_from_main");
         cJSON_AddBoolToObject(obj, "stale_index_risk", 1);
         return;
      }
      if (differs == 0)
      {
         cJSON_AddStringToObject(obj, "freshness", "worktree_matches_main");
         return;
      }
   }

   cJSON_AddStringToObject(obj, "freshness", "worktree_current_unverified");
}

int agent_source_append_overlay_code_hits(cJSON *arr, const char *query, const char *project,
                                          int max_results)
{
   (void)project;
   if (!arr || !source_authority_enabled() || !query || !query[0] || max_results <= 0)
      return 0;
   const char *paths = source_authority_paths();
   if (!paths || !paths[0])
      return 0;

   int added = 0;
   const char *p = paths;
   while (*p && added < max_results)
   {
      const char *nl = strchr(p, '\n');
      size_t len = nl ? (size_t)(nl - p) : strlen(p);
      char src_path[MAX_PATH_LEN] = "";
      if (len > 0)
      {
         size_t copy = len < sizeof(src_path) - 1 ? len : sizeof(src_path) - 1;
         memcpy(src_path, p, copy);
         src_path[copy] = '\0';
      }
      if (src_path[0])
      {
         char resolved[MAX_PATH_LEN];
         source_authority_resolve_path(src_path, resolved, sizeof(resolved));
         FILE *f = fopen(resolved, "r");
         if (f)
         {
            char line[4096];
            int line_no = 0;
            while (fgets(line, sizeof(line), f) && added < max_results)
            {
               line_no++;
               if (!line_matches_query(line, query))
                  continue;
               line[strcspn(line, "\r\n")] = '\0';
               cJSON *h = cJSON_CreateObject();
               cJSON_AddStringToObject(h, "project", "current_overlay");
               cJSON_AddStringToObject(h, "file", src_path);
               cJSON_AddNumberToObject(h, "line", line_no);
               cJSON_AddStringToObject(h, "snippet", line);
               cJSON_AddNumberToObject(h, "rank", 1000.0 - added);
               cJSON_AddStringToObject(h, "evidence", "source_packet");
               cJSON_AddStringToObject(h, "freshness", "source_packet_current");
               cJSON_AddStringToObject(h, "authority", "current_source");
               cJSON_AddStringToObject(h, "authority_note",
                                       "This hit came from source explicitly supplied to the "
                                       "delegate and overrides indexed snippets for content.");
               cJSON_AddItemToArray(arr, h);
               added++;
            }
            fclose(f);
         }
      }
      if (!nl)
         break;
      p = nl + 1;
   }
   return added;
}

char *tool_find_symbol(const char *identifier)
{
   if (!identifier || !identifier[0])
      return safe_strdup("error: missing identifier");

   term_hit_t hits[20];
   int count = kb_client_index_find(identifier, hits, 20);
   char buf[8192];
   int pos = 0;

   int overlay_matches = 0;
   if (source_authority_enabled())
   {
      const char *paths = source_authority_paths();
      const char *p = paths && paths[0] ? paths : NULL;
      while (p && *p && overlay_matches < 8 && pos < (int)sizeof(buf) - 512)
      {
         const char *nl = strchr(p, '\n');
         size_t len = nl ? (size_t)(nl - p) : strlen(p);
         char src_path[MAX_PATH_LEN] = "";
         if (len > 0)
         {
            size_t copy = len < sizeof(src_path) - 1 ? len : sizeof(src_path) - 1;
            memcpy(src_path, p, copy);
            src_path[copy] = '\0';
         }
         if (src_path[0])
         {
            char resolved[MAX_PATH_LEN];
            source_authority_resolve_path(src_path, resolved, sizeof(resolved));
            FILE *f = fopen(resolved, "r");
            if (f)
            {
               char line[4096];
               int line_no = 0;
               while (fgets(line, sizeof(line), f) && overlay_matches < 8 &&
                      pos < (int)sizeof(buf) - 512)
               {
                  line_no++;
                  if (!str_contains_ci(line, identifier))
                     continue;
                  if (overlay_matches == 0)
                     pos += snprintf(buf + pos, sizeof(buf) - pos,
                                     "Current source packet matches for '%s':\n\n", identifier);
                  pos += snprintf(buf + pos, sizeof(buf) - pos,
                                  "- %s:%d [source_packet_current; authority=current_source]\n",
                                  src_path, line_no);
                  overlay_matches++;
               }
               fclose(f);
            }
         }
         if (!nl)
            break;
         p = nl + 1;
      }
      if (overlay_matches > 0)
         pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
   }

   if (count <= 0)
   {
      if (overlay_matches <= 0)
         pos += snprintf(buf + pos, sizeof(buf) - pos, "No symbol found for '%s'", identifier);
   }
   else
   {
      pos += snprintf(buf + pos, sizeof(buf) - pos,
                      "Indexed discovery matches for '%s' (use current source/read_file for "
                      "file-content authority):\n\n",
                      identifier);
      for (int i = 0; i < count && pos < (int)sizeof(buf) - 512; i++)
      {
         cJSON *meta = cJSON_CreateObject();
         agent_source_add_index_freshness(meta, hits[i].project, hits[i].file_path);
         cJSON *fresh = cJSON_GetObjectItemCaseSensitive(meta, "freshness");
         cJSON *auth = cJSON_GetObjectItemCaseSensitive(meta, "authority");
         const char *freshness = cJSON_IsString(fresh) ? fresh->valuestring : "unknown";
         const char *authority = cJSON_IsString(auth) ? auth->valuestring : "discovery_only";
         pos += snprintf(buf + pos, sizeof(buf) - pos,
                         "- %s:%d [%s; freshness=%s; "
                         "authority=%s]\n",
                         hits[i].file_path, hits[i].line, hits[i].kind, freshness, authority);
         cJSON_Delete(meta);
      }
   }

   return safe_strdup(buf);
}

/* True if hit i duplicates an earlier hit (same file + start line + kind). Kind
 * is part of the key so two distinct definitions that happen to share a file:line
 * (e.g. a macro and a function) are surfaced as separate candidates, not merged. */
static int rs_is_dup(const term_hit_t *hits, int i)
{
   for (int j = 0; j < i; j++)
      if (hits[i].line == hits[j].line && strcmp(hits[i].file_path, hits[j].file_path) == 0 &&
          strcmp(hits[i].kind, hits[j].kind) == 0)
         return 1;
   return 0;
}

char *tool_read_symbol(const char *identifier, const char *sid)
{
   if (!identifier || !identifier[0])
      return safe_strdup("error: missing identifier");

   term_hit_t hits[20];
   int count = kb_client_index_find(identifier, hits, 20);
   if (count <= 0)
   {
      char e[256];
      snprintf(e, sizeof(e),
               "error: no indexed symbol found for '%s' (try find_symbol, grep, or read_file)",
               identifier);
      return safe_strdup(e);
   }

   int distinct = 0;
   for (int i = 0; i < count; i++)
      if (!rs_is_dup(hits, i))
         distinct++;

   /* Ambiguous: never guess — list the candidate definition sites and let the
    * caller pick (a more qualified name, or read the chosen span directly). */
   if (distinct > 1)
   {
      dstr_t d;
      dstr_init(&d);
      dstr_appendf(&d,
                   "'%s' resolves to %d definitions — disambiguate (use a more qualified name, or "
                   "read_file the chosen span):\n",
                   identifier, distinct);
      for (int i = 0; i < count; i++)
      {
         if (rs_is_dup(hits, i))
            continue;
         if (hits[i].line_end >= hits[i].line)
            dstr_appendf(&d, "  %s:%d-%d  [%s]\n", hits[i].file_path, hits[i].line,
                         hits[i].line_end, hits[i].kind[0] ? hits[i].kind : "symbol");
         else
            dstr_appendf(&d, "  %s:%d  [%.31s]\n", hits[i].file_path, hits[i].line,
                         hits[i].kind[0] ? hits[i].kind : "symbol");
      }
      return d.data ? d.data : safe_strdup("error: out of memory");
   }

   /* Unique definition: fetch just its span, anchored, so the caller can edit it
    * by reference without reading the whole file. Reuses the anchored read path
    * (whole-file snapshot + windowed anchored output). */
   const term_hit_t *h = &hits[0];
   if (!h->file_path[0])
      return safe_strdup("error: symbol has no file path in the index");
   int start = h->line >= 1 ? h->line : 1;
   int span = (h->line_end >= h->line) ? (h->line_end - h->line + 1) : 1;
   char *body = tool_read_file_ex(h->file_path, start - 1, span, 1 /*anchored*/, sid);

   dstr_t d;
   dstr_init(&d);
   dstr_appendf(&d, "symbol %s  [%.31s]  %s:%d%s\n", identifier, h->kind[0] ? h->kind : "symbol",
                h->file_path, start,
                (h->line_end >= h->line) ? "" : " (span end unknown; showing the definition line)");
   dstr_append_str(&d, body ? body : "");
   free(body);
   return d.data ? d.data : safe_strdup("error: out of memory");
}
