/* index.c: code indexing, project scanning, symbol lookup, blast radius analysis */
#include "aimee.h"
#include "config.h"
#include "css_analyze.h"
#include "modules/db2/c/code_index.h"
#include "modules/db2/c/css_graph.h"
#include "modules/db2/c/entity_edges.h"
#include "modules/db2/c/kb_runtime_state.h" /* co-change backfill idempotency marker */
#include "modules/memory/memory_ontology.h" /* REL_CO_EDITED / NODE_FILE */
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

static const char *get_extension(const char *path)
{
   const char *dot = strrchr(path, '.');
   if (!dot || dot == path)
      return "";
   return dot;
}

/* File list collected via find(1) */
typedef struct
{
   char **paths;
   int count;
   int max;
} file_list_t;

typedef struct
{
   char paths[64][256];
   int count;
} build_exclusion_list_t;

static int index_path_component_is_hidden(const char *component, size_t len)
{
   if (len == 0)
      return 0;
   return component[0] == '.';
}

static int index_path_has_hidden_component(const char *path)
{
   const char *p = path;
   while (*p == '/')
      p++;

   const char *start = p;
   for (;;)
   {
      if (*p == '/' || *p == '\0')
      {
         if (index_path_component_is_hidden(start, (size_t)(p - start)))
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

static int file_list_append(file_list_t *list, const char *path)
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

static void index_purge_hidden_paths(int64_t project_id)
{
   (void)db2_code_index_purge_files_matching(project_id, ".%");
   (void)db2_code_index_purge_files_matching(project_id, "%/.%");
}

static int ascii_contains(const char *haystack, const char *needle)
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

static int key_matches_output_subject(const char *key, const char *subject)
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
      if (*after == '\0' || !isalnum((unsigned char)*after) || ascii_contains(after, "DIR") ||
          ascii_contains(after, "PATH") || ascii_contains(after, "FILE"))
         return 1;
   }
   return 0;
}

static int build_key_is_output(const char *key)
{
   int subject =
       key_matches_output_subject(key, "BUILD") || key_matches_output_subject(key, "OBJ") ||
       key_matches_output_subject(key, "OUT") || key_matches_output_subject(key, "OUTPUT") ||
       key_matches_output_subject(key, "DIST") || key_matches_output_subject(key, "TARGET") ||
       key_matches_output_subject(key, "GEN") || key_matches_output_subject(key, "GENERATED");
   int shape =
       ascii_contains(key, "DIR") || ascii_contains(key, "PATH") || ascii_contains(key, "FILE");
   return subject && shape;
}

static void trim_manifest_token(char *token)
{
   while (*token == '"' || *token == '\'' || *token == '`')
      memmove(token, token + 1, strlen(token));
   size_t len = strlen(token);
   while (len > 0 && (token[len - 1] == '"' || token[len - 1] == '\'' || token[len - 1] == '`' ||
                      token[len - 1] == ';' || token[len - 1] == ',' || token[len - 1] == ')'))
      token[--len] = '\0';
}

static void build_exclusions_add(build_exclusion_list_t *exclusions, const char *base_dir,
                                 const char *token, int force_dir)
{
   if (exclusions->count >= (int)(sizeof(exclusions->paths) / sizeof(exclusions->paths[0])) ||
       !token || !*token || token[0] == '/' || strstr(token, "://") || strchr(token, '$') ||
       strchr(token, '*') || strchr(token, '%') || strchr(token, '&'))
      return;

   while (strncmp(token, "./", 2) == 0)
      token += 2;
   if (!*token || index_path_has_hidden_component(token))
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

static int path_is_build_excluded(const build_exclusion_list_t *exclusions, const char *rel_path)
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

static void parse_build_assignment(build_exclusion_list_t *exclusions, const char *base_dir,
                                   const char *key, char *value)
{
   if (!build_key_is_output(key))
      return;

   char *comment = strchr(value, '#');
   if (comment)
      *comment = '\0';

   int force_dir = ascii_contains(key, "DIR") || ascii_contains(key, "PATH");
   char *save = NULL;
   for (char *tok = strtok_r(value, " \t\r\n", &save); tok; tok = strtok_r(NULL, " \t\r\n", &save))
   {
      trim_manifest_token(tok);
      build_exclusions_add(exclusions, base_dir, tok, force_dir);
   }
}

static void parse_build_manifest(const char *root, const char *path,
                                 build_exclusion_list_t *exclusions)
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

      parse_build_assignment(exclusions, base_dir, key, eq + 1);
   }
   fclose(f);
}

static void collect_build_exclusions(const char *root, build_exclusion_list_t *exclusions)
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
      if (!line[0] || index_path_has_hidden_component(line))
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
      parse_build_manifest(root, path, exclusions);
   }
   free(out);
}

static void purge_build_exclusions(int64_t project_id, const build_exclusion_list_t *exclusions)
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

/* A project is fully indexed, but it never indexes its sub-projects: any nested
 * git repository under the root is its own project and is scanned separately.
 * We treat "directory contains a .git entry (file or dir)" as the sole
 * sub-project boundary, so submodules, embedded clones, and linked worktrees
 * are all handled the same way. */
#define SUBPROJECT_PREFIX_CAP 256
#define SUBPROJECT_MAX_DEPTH  10

static int index_is_skip_dir(const char *name)
{
   static const char *skip[] = {"node_modules", ".git",   "vendor",     "__pycache__", "build",
                                "dist",         "target", ".worktrees", ".aimee",      "bin",
                                "obj",          ".cache", ".venv",      "venv",        ".tox",
                                "coverage",     "sdks",   NULL};
   for (int i = 0; skip[i]; i++)
      if (strcmp(name, skip[i]) == 0)
         return 1;
   return 0;
}

static int dir_contains_git(const char *path)
{
   char git_path[MAX_PATH_LEN];
   struct stat st;
   snprintf(git_path, sizeof(git_path), "%s/.git", path);
   return stat(git_path, &st) == 0;
}

/* Collect root-relative prefixes (each with a trailing '/') of nested git repos
 * found under `cur`. Files under any of these prefixes belong to a sub-project
 * and must not be indexed as part of the parent project. Descent stops at each
 * sub-project boundary and at the usual build/vendor/hidden directories. */
static void collect_subproject_prefixes(const char *root, const char *cur, int depth,
                                        char prefixes[][MAX_PATH_LEN], int *count)
{
   if (depth > SUBPROJECT_MAX_DEPTH || *count >= SUBPROJECT_PREFIX_CAP)
      return;

   DIR *d = opendir(cur);
   if (!d)
      return;

   size_t root_len = strlen(root);
   struct dirent *ent;
   while ((ent = readdir(d)) != NULL && *count < SUBPROJECT_PREFIX_CAP)
   {
      if (ent->d_name[0] == '.')
         continue;
      if (index_is_skip_dir(ent->d_name))
         continue;

      char sub[MAX_PATH_LEN];
      snprintf(sub, sizeof(sub), "%s/%s", cur, ent->d_name);

      struct stat st;
      if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode))
         continue;

      if (dir_contains_git(sub))
      {
         /* sub is a nested sub-project root; record it and do not descend. */
         const char *rel = sub + root_len;
         while (*rel == '/')
            rel++;
         if (*rel)
            snprintf(prefixes[(*count)++], MAX_PATH_LEN, "%s/", rel);
      }
      else
      {
         collect_subproject_prefixes(root, sub, depth + 1, prefixes, count);
      }
   }
   closedir(d);
}

static int path_in_subproject(const char *policy_path, char prefixes[][MAX_PATH_LEN], int count)
{
   for (int i = 0; i < count; i++)
   {
      if (strncmp(policy_path, prefixes[i], strlen(prefixes[i])) == 0)
         return 1;
   }
   return 0;
}

/* Drop any rows previously indexed under this project that actually belong to a
 * nested sub-project, so a rescan cleans up files a parent used to absorb. */
static void purge_subprojects(int64_t project_id, const char *root)
{
   char(*prefixes)[MAX_PATH_LEN] = malloc(sizeof(*prefixes) * SUBPROJECT_PREFIX_CAP);
   if (!prefixes)
      return;
   int count = 0;
   collect_subproject_prefixes(root, root, 0, prefixes, &count);
   char pattern[MAX_PATH_LEN];
   for (int i = 0; i < count; i++)
   {
      snprintf(pattern, sizeof(pattern), "%s%%", prefixes[i]);
      (void)db2_code_index_purge_files_matching(project_id, pattern);
   }
   free(prefixes);
}

/* Collect code files from git's tracked + unignored candidate set, then
 * apply index policy: hidden path components are never source, and build
 * outputs are excluded when build manifests declare them as generated
 * locations. For directories outside git we use the same hidden/manifest
 * policy over a find-based walk. Files are then filtered by extractor and
 * size. */
static void collect_code_files(const char *dir, const char *root, file_list_t *list)
{
   build_exclusion_list_t build_exclusions = {0};
   collect_build_exclusions(root, &build_exclusions);

   /* Heap-allocated (256 * 4 KiB = 1 MiB) to keep this scan worker's stack
    * frame small; collect_code_files runs on pooled threads. */
   char(*subproject_prefixes)[MAX_PATH_LEN] =
       malloc(sizeof(*subproject_prefixes) * SUBPROJECT_PREFIX_CAP);
   int subproject_count = 0;
   if (subproject_prefixes)
      collect_subproject_prefixes(root, root, 0, subproject_prefixes, &subproject_count);

   char *esc = shell_escape(dir);
   char cmd[MAX_PATH_LEN * 2 + 256];
   int rc;

   /* Probe: is `dir` inside a git work tree? */
   snprintf(cmd, sizeof(cmd), "git -C %s rev-parse --is-inside-work-tree 2>/dev/null", esc);
   char *probe = run_cmd(cmd, &rc);
   int in_git = (rc == 0 && probe && strncmp(probe, "true", 4) == 0);
   free(probe);

   char *out = NULL;
   if (in_git)
   {
      /* `--cached --others --exclude-standard` = tracked + untracked-not-ignored.
       * Paths are repo-relative; we expand to absolute below. */
      snprintf(cmd, sizeof(cmd),
               "git -C %s ls-files --cached --others --exclude-standard 2>/dev/null", esc);
      out = run_cmd(cmd, &rc);
   }
   else
   {
      /* Non-git fallback: keep the historical path-glob walk so non-git
       * workspaces (docs trees, scratch dirs) still get indexed. */
      snprintf(cmd, sizeof(cmd),
               "find %s -type f"
               " ! -name '.*'"
               " ! -path '*/.*/*'"
               " -size -%dc 2>/dev/null",
               esc, MAX_FILE_SIZE + 1);
      out = run_cmd(cmd, &rc);
   }
   free(esc);
   if (!out)
   {
      free(subproject_prefixes);
      return;
   }

   size_t dir_len = strlen(dir);
   for (char *line = out, *nl; line && *line; line = nl ? nl + 1 : NULL)
   {
      nl = strchr(line, '\n');
      if (nl)
         *nl = '\0';
      if (!line[0])
         continue;

      char relbuf[MAX_PATH_LEN];
      const char *policy_path = line;
      if (!in_git && strncmp(line, root, dir_len) == 0)
      {
         const char *rel = line + dir_len;
         if (*rel == '/')
            rel++;
         snprintf(relbuf, sizeof(relbuf), "%s", rel);
         policy_path = relbuf;
      }

      if (index_path_has_hidden_component(policy_path))
         continue;
      if (path_in_subproject(policy_path, subproject_prefixes, subproject_count))
         continue;
      if (path_is_build_excluded(&build_exclusions, policy_path))
         continue;
      if (!index_has_extractor(get_extension(line)))
         continue;

      /* git ls-files emits repo-relative paths; expand to absolute so the
       * downstream scanner can stat / read them. The find path already
       * emits absolute paths. */
      char full[MAX_PATH_LEN];
      const char *abs_path;
      if (in_git)
      {
         snprintf(full, sizeof(full), "%.*s/%s", (int)dir_len, dir, line);
         abs_path = full;
      }
      else
      {
         abs_path = line;
      }

      /* Size + regular-file guard. The find path already filters by
       * size; the git path needs an explicit stat because ls-files
       * doesn't know how big a file is. */
      struct stat st;
      if (stat(abs_path, &st) != 0 || !S_ISREG(st.st_mode))
         continue;
      if (st.st_size > MAX_FILE_SIZE)
         continue;

      if (file_list_append(list, abs_path) != 0)
         break;
   }
   free(out);
   free(subproject_prefixes);
}

static char *read_file_content(const char *path, size_t *out_len)
{
   FILE *f = fopen(path, "r");
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

   char *buf = malloc(len + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }

   size_t nread = fread(buf, 1, len, f);
   buf[nread] = '\0';
   fclose(f);

   if (out_len)
      *out_len = nread;
   return buf;
}

/* --- Git co-change backfill ---
 *
 * Files that change together in a commit are coupled in ways the static call
 * graph cannot see (shared wire formats, ordering assumptions, implicit
 * contracts). We mine that from git history and fold it into the SAME
 * co_edited edges the in-session co-edit path writes and index_blast_radius
 * already reads (keyed by basename, weight = co-change count). No new table,
 * no new reader — history just seeds the graph that was previously populated
 * only from aimee's own edit windows. */

#define COCHANGE_MAX_FILES  25  /* bulk-commit gate: skip sweeps/squashes above this */
#define COCHANGE_NAME_CAP   64  /* per-commit distinct basenames we bother to hold */
#define COCHANGE_CKPT_EVERY 200 /* commits between marker checkpoints (crash re-mine bound) */

/* cochange_pairs_for_commit() (the pure pairing policy) lives in cochange.c and
 * is declared in index.h. */

static void cochange_flush(char names[][128], int ncount, cochange_pair_t *pairs, int max_pairs)
{
   int np = cochange_pairs_for_commit(names, ncount, COCHANGE_MAX_FILES, pairs, max_pairs);
   for (int p = 0; p < np; p++)
   {
      int added = 0;
      /* window_id 0: git-history provenance (no session window). Upsert bumps
       * weight on repeat, so an N-times co-changed pair reaches weight N. */
      db2_entity_edge_upsert(pairs[p].a, "co_edited", pairs[p].b, 0, (int)REL_CO_EDITED,
                             (int)NODE_FILE, (int)NODE_FILE, &added);
   }
}

/* Mine git history under `abs_root` into co_edited edges. Incremental: only
 * commits after the last synced HEAD (a per-project marker in kb_runtime_state)
 * are processed, so re-scans never double-count. After a rebase/force-update
 * that orphans the marker, we resync it forward without re-mining (re-mining
 * would double the accumulated weights). Silent no-op outside a git work tree. */
static void index_backfill_cochange(const char *project, const char *abs_root)
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
   if (!cochange_is_hex_sha(head)) /* never store/interpolate a non-object-id */
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
      /* Corrupt/edited kb_runtime_state row: the marker is interpolated into a
       * git command below, so an unvalidated value must never reach the shell.
       * Resync forward without mining (same fail-safe as an orphaned marker). */
      db2_kb_runtime_state_set(key, head);
      free(head);
      free(esc);
      return;
   }

   char revspec[160] = "";
   if (have_marker)
   {
      if (strcmp(marker, head) == 0) /* already synced to this HEAD */
      {
         free(head);
         free(esc);
         return;
      }
      /* marker is already validated to lowercase-hex above, so it is shell-inert;
       * shell_escape it anyway so every value interpolated into a git command is
       * escaped uniformly (matching abs_root), leaving no unescaped interpolation. */
      char *emarker = shell_escape(marker);
      const char *m = emarker ? emarker : marker;
      snprintf(cmd, sizeof(cmd), "git -C %s merge-base --is-ancestor %s HEAD 2>/dev/null", esc, m);
      char *anc = run_cmd(cmd, &rc);
      free(anc);
      if (rc != 0) /* marker orphaned (rebase/force-update): resync, do not re-mine */
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

   /* --reverse: process oldest->newest so the checkpoint marker only ever
    * advances to a commit that is already an ancestor of HEAD.
    * -M with --name-only emits a renamed file as a SINGLE (post-rename) path, not
    * an old/new pair, so a rename yields no spurious old<->new co-change edge. */
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

   const int max_pairs = COCHANGE_MAX_FILES * (COCHANGE_MAX_FILES - 1) / 2;
   cochange_pair_t *pairs = malloc(sizeof(*pairs) * (size_t)max_pairs);
   if (!pairs)
   {
      free(log);
      free(head);
      return;
   }

   char names[COCHANGE_NAME_CAP][128];
   int ncount = 0;
   int in_commit = 0;
   char cur_sha[128] = ""; /* commit whose files we are collecting */
   int done = 0;           /* commits fully flushed since the last checkpoint */
   for (char *line = log, *nl; line && *line; line = nl ? nl + 1 : NULL)
   {
      nl = strchr(line, '\n');
      if (nl)
         *nl = '\0';
      if (line[0] == '@' && line[1] == '@') /* new commit boundary */
      {
         if (in_commit)
         {
            cochange_flush(names, ncount, pairs, max_pairs);
            /* Checkpoint the marker to the just-finished commit every
             * COCHANGE_CKPT_EVERY commits. A crash then re-mines at most that
             * many commits on the next scan instead of the whole range, so the
             * edge weights stay bounded (--reverse keeps cur_sha an ancestor of
             * HEAD, so the resume ancestor-check still holds). */
            if (cochange_is_hex_sha(cur_sha) && ++done >= COCHANGE_CKPT_EVERY)
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
      const char *ext = get_extension(base);
      if (!ext || !index_has_extractor(ext))
         continue;
      if (ncount >= COCHANGE_NAME_CAP) /* commit already over the gate; stop collecting */
         continue;
      snprintf(names[ncount], sizeof(names[0]), "%s", base);
      ncount++;
   }
   if (in_commit)
      cochange_flush(names, ncount, pairs, max_pairs);

   /* Final marker = HEAD covers the whole processed range (the last --reverse
    * commit is HEAD itself). */
   db2_kb_runtime_state_set(key, head);

   free(pairs);
   free(log);
   free(head);
}

int index_scan_project(const char *name, const char *root, int force)
{
   /* Resolve to absolute path */
   char abs_root[MAX_PATH_LEN];
   if (realpath(root, abs_root) == NULL)
      snprintf(abs_root, sizeof(abs_root), "%s", root);

   if (index_path_has_hidden_component(abs_root))
   {
      fprintf(stderr, "aimee index: refusing to scan hidden root: %s\n", abs_root);
      return -1;
   }

   int64_t project_id = db2_code_index_project_upsert(name, abs_root);
   if (project_id < 0)
      return -1;

   /* CSS migration assistant (WP-C): the style-graph write path is opt-in. Read
    * the flag once per scan; off by default, the indexer keeps only the legacy
    * lexical CSS class-name scan (file_exports). */
   int css_graph_on = config_css_style_graph_enabled();
   int cochange_on = config_code_cochange_git_enabled();

   build_exclusion_list_t build_exclusions = {0};
   collect_build_exclusions(abs_root, &build_exclusions);
   index_purge_hidden_paths(project_id);
   purge_build_exclusions(project_id, &build_exclusions);
   purge_subprojects(project_id, abs_root);

   char ts[32];
   now_utc(ts, sizeof(ts));

   /* Collect all code files */
   file_list_t list = {0};
   collect_code_files(abs_root, abs_root, &list);

   int scanned = 0;
   for (int i = 0; i < list.count; i++)
   {
      const char *full = list.paths[i];

      /* Compute relative path */
      const char *rel = full + strlen(abs_root);
      if (*rel == '/')
         rel++;

      /* Check mtime vs stored scanned_at */
      struct stat st;
      if (stat(full, &st) != 0)
      {
         free(list.paths[i]);
         continue;
      }

      if (!force && !db2_code_index_file_modified_since(project_id, rel, st.st_mtime))
      {
         free(list.paths[i]);
         continue;
      }

      /* Read content */
      size_t content_len;
      char *content = read_file_content(full, &content_len);
      if (!content)
      {
         free(list.paths[i]);
         continue;
      }

      /* Upsert file record and get file_id */
      int64_t file_id = db2_code_index_file_upsert(project_id, rel, ts);
      if (file_id < 0)
      {
         free(content);
         free(list.paths[i]);
         continue;
      }

      /* Extract derived data and atomically replace per-file index rows. */
      const char *ext = get_extension(full);
      char *exports[128];
      int exp_count = extract_exports(ext, content, exports, 128);
      char *imports[128];
      int imp_count = extract_imports(ext, content, imports, 128);
      char *routes[64];
      int route_count = extract_routes(ext, content, routes, 64);
      definition_t defs[256];
      int def_count = extract_definitions(ext, content, defs, 256);
      call_ref_t calls[512];
      int call_count = extract_calls(ext, content, calls, 512);

      code_index_file_data_t data = {
          .content = content,
          .exports = exports,
          .export_count = exp_count,
          .imports = imports,
          .import_count = imp_count,
          .routes = routes,
          .route_count = route_count,
          .definitions = defs,
          .definition_count = def_count,
          .calls = calls,
          .call_count = call_count,
      };
      db2_code_index_file_replace(file_id, &data);

      /* WP-C: build the CSS style graph for .css files (plain CSS only; SCSS is
       * indexed from its compiled output, not source). The lexical class-name
       * scan above is preserved for backward compatibility. */
      if (css_graph_on && ext && strcmp(ext, ".css") == 0)
      {
         css_stylesheet_t *ss = css_analyze(content, content_len);
         if (ss)
         {
            (void)db2_css_graph_replace(file_id, ss->rules, ss->rule_count);
            css_stylesheet_free(ss);
         }
      }
      /* WP-D: component -> style join. For markup-bearing files, resolve the
       * static class tokens against the style graph (dynamic classes are skipped
       * -> left unresolved, no silent miss). */
      else if (css_graph_on && ext &&
               (strcmp(ext, ".tsx") == 0 || strcmp(ext, ".jsx") == 0 || strcmp(ext, ".ts") == 0 ||
                strcmp(ext, ".js") == 0 || strcmp(ext, ".html") == 0 || strcmp(ext, ".vue") == 0 ||
                strcmp(ext, ".svelte") == 0))
      {
         static char class_tokens[512][CSS_CLASS_TOKEN_MAX];
         int nt = css_extract_class_tokens(content, content_len, class_tokens, 512);
         if (nt > 0)
            (void)db2_css_component_resolve(file_id, class_tokens, nt);
      }

      for (int j = 0; j < exp_count; j++)
         free(exports[j]);
      for (int j = 0; j < imp_count; j++)
         free(imports[j]);
      for (int j = 0; j < route_count; j++)
         free(routes[j]);

      free(content);
      free(list.paths[i]);
      scanned++;
   }

   free(list.paths);

   /* Seed co_edited edges from git history (incremental after the first scan). */
   if (cochange_on)
      index_backfill_cochange(name, abs_root);

   return scanned;
}

int index_list_projects(project_info_t *out, int max)
{
   return db2_code_index_project_list(out, max);
}

int index_find(const char *identifier, term_hit_t *out, int max)
{
   return db2_code_index_term_find(identifier, out, max);
}

int index_blast_radius(const char *project, const char *file_path, blast_radius_t *out)
{
   if (!project || !file_path || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   snprintf(out->file, sizeof(out->file), "%s", file_path);

   if (db2_code_index_blast_radius(project, file_path, out) != 0)
      return -1;

   /* Expand with co_edited graph edges only when their legacy basename key
    * resolves to exactly one current-project file. */
   {
      const char *base = strrchr(file_path, '/');
      const char *match_name = base ? base + 1 : file_path;

      char co_buf[16][128];
      int n = db2_entity_edge_co_targets(match_name, "co_edited", 3, co_buf, 16);
      for (int b = 0; b < n && out->dependent_count < 64; b++)
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
            if (out->dependent_count >= 64)
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

int index_structure(const char *project, const char *file_path, definition_t *out, int max)
{
   return db2_code_index_file_definitions(project, file_path, out, max);
}

/* --- Blast radius preview for multiple files --- */

int index_find_callers(const char *project, const char *symbol, caller_hit_t *out, int max)
{
   return db2_code_index_callers_find(project, symbol, out, max);
}

int index_code_search(const char *query, const char *project, code_search_hit_t *out, int max)
{
   /* The direct (non-HTTP) code-search path does not enrich line spans. */
   return db2_code_index_code_search(query, project, out, max, 0);
}
