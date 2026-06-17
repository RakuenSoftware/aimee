/* workspace.c: directory-based workspace discovery and context generation */
#include "aimee.h"
#include "log.h"
#include "workspace.h"
#include "index.h"
#include "kb_client.h"
#include "headers/branch_ownership.h"
#include "headers/config.h"
#include "headers/platform_path.h"
#include "headers/util.h"
#include <dirent.h>
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- recursive git discovery --- */

/* Directories to skip during discovery */
static int is_skip_dir(const char *name)
{
   static const char *skip[] = {"node_modules", ".git",   "vendor",     "__pycache__", "build",
                                "dist",         "target", ".worktrees", ".aimee",      "bin",
                                "obj",          ".cache", ".venv",      "venv",        ".tox",
                                "coverage",     "sdks",   NULL};
   for (int i = 0; skip[i]; i++)
   {
      if (strcmp(name, skip[i]) == 0)
         return 1;
   }
   return 0;
}

/* Classify a directory's .git entry: 0 = not a repo, 1 = real checkout (.git is
 * a directory), 2 = linked worktree (.git is a regular file: "gitdir: <path>").
 * Linked worktrees are duplicate working copies of an already-tracked repo, so
 * auto-discovery skips them (see discover_recursive) to avoid indexing the same
 * codebase once per worktree. */
static int git_dir_kind(const char *path)
{
   char git_path[MAX_PATH_LEN];
   struct stat st;
   snprintf(git_path, sizeof(git_path), "%s/.git", path);
   if (stat(git_path, &st) != 0)
      return 0;
   return S_ISDIR(st.st_mode) ? 1 : 2;
}

static void discover_recursive(const char *dir, int depth, int max_depth,
                               char projects[][MAX_PATH_LEN], int max, int *count)
{
   if (depth > max_depth || *count >= max)
      return;

   /* Register this directory as a project if it's a git repo. A real checkout
    * (.git dir) always counts. A linked worktree (.git file) is a second working
    * copy of a repo that's tracked elsewhere — counting each one re-indexes the
    * same codebase N times — so it's only honored when added explicitly (the
    * root of the scan, depth 0), never as a sibling found during descent. */
   int gk = git_dir_kind(dir);
   if (gk == 1 || (gk == 2 && depth == 0))
   {
      char abs[MAX_PATH_LEN];
      if (realpath(dir, abs))
         snprintf(projects[(*count)++], MAX_PATH_LEN, "%s", abs);
      else
         snprintf(projects[(*count)++], MAX_PATH_LEN, "%s", dir);
      /* Keep recursing: a project may itself contain nested git repos, and each
       * of those is its own separate project (a project never absorbs its
       * sub-projects). The skip/hidden/max_depth guards below still apply. */
   }

   DIR *d = opendir(dir);
   if (!d)
      return;

   struct dirent *ent;
   while ((ent = readdir(d)) != NULL && *count < max)
   {
      if (ent->d_name[0] == '.')
         continue;
      if (is_skip_dir(ent->d_name))
         continue;

      char sub[MAX_PATH_LEN];
      snprintf(sub, sizeof(sub), "%s/%s", dir, ent->d_name);

      struct stat st;
      if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode))
         continue;

      discover_recursive(sub, depth + 1, max_depth, projects, max, count);
   }
   closedir(d);
}

int workspace_discover_projects(const char *root, int max_depth, char projects[][MAX_PATH_LEN],
                                int max)
{
   if (!root || !root[0])
      return -1;

   char abs_root[MAX_PATH_LEN];
   if (!realpath(root, abs_root))
      return -1;

   int count = 0;
   discover_recursive(abs_root, 0, max_depth, projects, max, &count);
   return count;
}

int workspace_active_root(const config_t *cfg, const char *cwd, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return -1;
   out[0] = '\0';

   if (cwd && cwd[0])
   {
      char cmd[MAX_PATH_LEN + 128];
      int rc = 0;
      snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse --show-toplevel 2>/dev/null", cwd);
      char *git_root = run_cmd(cmd, &rc);
      if (rc == 0 && git_root && git_root[0])
      {
         size_t len = strlen(git_root);
         while (len > 0 && (git_root[len - 1] == '\n' || git_root[len - 1] == '\r'))
            git_root[--len] = '\0';
         if (git_root[0])
         {
            snprintf(out, out_len, "%s", git_root);
            free(git_root);
            return 0;
         }
      }
      free(git_root);

      char resolved_cwd[MAX_PATH_LEN];
      const char *cwd_abs = realpath(cwd, resolved_cwd) ? resolved_cwd : cwd;

      if (cfg)
      {
         int best = -1;
         size_t best_len = 0;
         for (int i = 0; i < cfg->workspace_count; i++)
         {
            const char *ws = cfg->workspaces[i];
            size_t ws_len = strlen(ws);
            if (ws_len == 0)
               continue;
            if (strncmp(cwd_abs, ws, ws_len) == 0 &&
                (cwd_abs[ws_len] == '/' || cwd_abs[ws_len] == '\0'))
            {
               if (ws_len > best_len)
               {
                  best = i;
                  best_len = ws_len;
               }
            }
         }
         if (best >= 0)
         {
            snprintf(out, out_len, "%s", cfg->workspaces[best]);
            return 0;
         }
      }

      snprintf(out, out_len, "%s", cwd_abs);
      return 0;
   }

   if (cfg && cfg->workspace_count > 0 && cfg->workspaces[0][0])
   {
      snprintf(out, out_len, "%s", cfg->workspaces[0]);
      return 0;
   }

   return -1;
}

/* --- context generation --- */

#ifndef AIMEE_DB1_DISABLED
/* Forward declaration: reads project description from ~/.config/aimee/projects/<name>.md */
char *describe_read(const char *project_name);
#endif

char *style_read(const char *project_name)
{
   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/projects/%s.style.md", config_default_dir(), project_name);

   FILE *f = fopen(path, "r");
   if (!f)
      return NULL;

   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);

   if (sz <= 0 || sz > MAX_FILE_SIZE)
   {
      fclose(f);
      return NULL;
   }

   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }

   size_t n = fread(buf, 1, (size_t)sz, f);
   buf[n] = '\0';
   fclose(f);
   return buf;
}

#ifndef AIMEE_DB1_DISABLED
/* Strip frontmatter (--- ... ---) from a description/style string.
 * Returns pointer into the same buffer (does not allocate). */
static const char *skip_frontmatter(const char *content)
{
   if (strncmp(content, "---", 3) != 0)
      return content;
   const char *end = strstr(content + 3, "---");
   if (!end)
      return content;
   end += 3;
   while (*end == '\n' || *end == '\r')
      end++;
   return end;
}
#endif

char *workspace_build_context_from_config(const config_t *cfg)
{
#ifdef AIMEE_DB1_DISABLED
   (void)cfg;
   return NULL;
#else
   size_t bufsize = 64 * 1024;
   char *buf = malloc(bufsize);
   if (!buf)
      return NULL;

   size_t pos = 0;
   /* Safe snprintf helper: advance pos but never past bufsize-1 */
#define BUF_PRINTF(...)                                                                            \
   do                                                                                              \
   {                                                                                               \
      if (pos < bufsize - 1)                                                                       \
      {                                                                                            \
         int _n = snprintf(buf + pos, bufsize - pos, __VA_ARGS__);                                 \
         if (_n > 0)                                                                               \
            pos += ((size_t)_n < bufsize - pos) ? (size_t)_n : bufsize - pos - 1;                  \
      }                                                                                            \
   } while (0)

   /* Get all indexed projects */
   project_info_t projects[256];
   int pcount = kb_client_index_list(projects, 256);

   /* Group projects by workspace and emit context */
   for (int w = 0; w < cfg->workspace_count; w++)
   {
      const char *ws_root = cfg->workspaces[w];
      size_t ws_len = strlen(ws_root);

      /* Find projects belonging to this workspace */
      int first = 1;
      for (int p = 0; p < pcount; p++)
      {
         /* Match: project root starts with workspace root */
         if (strncmp(projects[p].root, ws_root, ws_len) != 0)
            continue;
         /* Must be exact match or followed by '/' */
         if (projects[p].root[ws_len] != '/' && projects[p].root[ws_len] != '\0')
            continue;

         if (first)
         {
            BUF_PRINTF("# Workspace: %s\n\n", ws_root);
            first = 0;
         }

         /* Read and include project description */
         char *desc = describe_read(projects[p].name);
         if (desc)
         {
            const char *content = skip_frontmatter(desc);
            size_t desc_len = strlen(content);
            if (pos + desc_len + 4 < bufsize)
            {
               memcpy(buf + pos, content, desc_len);
               pos += desc_len;
               if (pos > 0 && buf[pos - 1] != '\n')
                  buf[pos++] = '\n';
               buf[pos++] = '\n';
            }
            free(desc);
         }
         else
         {
            BUF_PRINTF("## %s\n(no description)\n\n", projects[p].name);
         }

         /* Read and include style guide */
         char *style = style_read(projects[p].name);
         if (style)
         {
            const char *content = skip_frontmatter(style);
            size_t style_len = strlen(content);
            if (pos + style_len + 4 < bufsize)
            {
               memcpy(buf + pos, content, style_len);
               pos += style_len;
               if (pos > 0 && buf[pos - 1] != '\n')
                  buf[pos++] = '\n';
               buf[pos++] = '\n';
            }
            free(style);
         }
      }
   }

   /* Also include projects not belonging to any workspace */
   for (int p = 0; p < pcount; p++)
   {
      int found_ws = 0;
      for (int w = 0; w < cfg->workspace_count; w++)
      {
         size_t ws_len = strlen(cfg->workspaces[w]);
         if (strncmp(projects[p].root, cfg->workspaces[w], ws_len) == 0 &&
             (projects[p].root[ws_len] == '/' || projects[p].root[ws_len] == '\0'))
         {
            found_ws = 1;
            break;
         }
      }
      if (found_ws)
         continue;

      char *desc = describe_read(projects[p].name);
      if (desc)
      {
         const char *content = skip_frontmatter(desc);
         size_t desc_len = strlen(content);
         if (pos + desc_len + 4 < bufsize)
         {
            memcpy(buf + pos, content, desc_len);
            pos += desc_len;
            if (pos > 0 && buf[pos - 1] != '\n')
               buf[pos++] = '\n';
            buf[pos++] = '\n';
         }
         free(desc);
      }
   }

   if (pos >= bufsize)
      pos = bufsize - 1;
   buf[pos] = '\0';
#undef BUF_PRINTF
   return buf;
#endif
}

/* --- resolve_proposal_path --- */

char *resolve_proposal_path(const char *proposal)
{
   if (!proposal || !proposal[0])
      return NULL;

   /* 1. Try path as is (might be absolute or relative to CWD) */
   if (access(proposal, R_OK) == 0)
      return realpath(proposal, NULL);

   /* 2. Try from config workspaces */
   config_t cfg;
   if (config_load(&cfg) == 0)
   {
      for (int w = 0; w < cfg.workspace_count; w++)
      {
         char path[MAX_PATH_LEN];
         snprintf(path, sizeof(path), "%s/%s", cfg.workspaces[w], proposal);
         if (access(path, R_OK) == 0)
            return realpath(path, NULL);
      }
   }

   /* 3. Search docs/proposals/ subdirectories for the filename */
   const char *filename = strrchr(proposal, '/');
   if (filename)
      filename++;
   else
      filename = proposal;

   static const char *subdirs[] = {"pending", "accepted", "done", "rejected",
                                   "reviews", "deferred", NULL};
   char search_path[MAX_PATH_LEN];

   for (int i = 0; subdirs[i]; i++)
   {
      /* Relative to CWD */
      snprintf(search_path, sizeof(search_path), "docs/proposals/%s/%s", subdirs[i], filename);
      if (access(search_path, R_OK) == 0)
         return realpath(search_path, NULL);

      /* Relative to each workspace root */
      for (int w = 0; w < cfg.workspace_count; w++)
      {
         snprintf(search_path, sizeof(search_path), "%s/docs/proposals/%s/%s", cfg.workspaces[w],
                  subdirs[i], filename);
         if (access(search_path, R_OK) == 0)
            return realpath(search_path, NULL);
      }
   }

   return NULL;
}

/* --- Worktree Lifecycle --- */
#include <unistd.h>

/* Compute the expected aimee-managed worktree path for a git repo and session.
 * For git_root="/root/dev/aimee" and session "abc123...", produces
 * "/root/dev/aimee/.aimee/worktrees/abc12345/main".
 * If work_name is non-NULL (e.g. "task01"), produces
 * "/root/dev/aimee/.aimee/worktrees/abc12345/task01" to avoid collisions
 * when multiple delegates run in the same session. */
int worktree_sibling_path(const char *git_root, const char *sid, const char *work_name,
                          char *wt_buf, size_t wt_len)
{
   if (!git_root || !sid || !wt_buf)
      return -1;

   /* Use first 8 chars of session ID */
   char short_id[12];
   snprintf(short_id, sizeof(short_id), "%.8s", sid);

   if (work_name && work_name[0])
      snprintf(wt_buf, wt_len, "%s/.aimee/worktrees/%s/%s", git_root, short_id, work_name);
   else
      snprintf(wt_buf, wt_len, "%s/.aimee/worktrees/%s/main", git_root, short_id);
   return 0;
}

/* Deterministic per-delegation worktree work-name, derived from the session id.
 *
 * A single delegation used to provision TWO sibling worktrees: the dispatch path
 * (cmd_agent_delegate.c) and the server compute path (server_compute.c) each
 * generated an INDEPENDENT random work-name, so they created two worktrees under
 * the same session dir — one of them stale/orphaned, and the delegate could end
 * up running in the wrong (divergent-base) one. Deriving the work-name
 * deterministically from the session id makes both paths resolve to the SAME
 * sibling worktree, so whichever runs second idempotently reuses the first's
 * worktree (worktree_create_sibling_at_ref returns the existing one) instead of
 * spawning a duplicate. The hash is over the first 8 chars of the sid — exactly
 * the component both paths already share as the worktree directory — so the two
 * call sites agree even if their full sid strings differ. */
int worktree_delegate_work_name(const char *sid, char *out, size_t cap)
{
   if (!sid || !out || cap < 9)
      return -1;
   char short_id[12];
   snprintf(short_id, sizeof(short_id), "%.8s", sid);
   /* FNV-1a (32-bit) over the short id — stable across processes and builds. */
   uint32_t h = 2166136261u;
   for (const char *p = short_id; *p; p++)
   {
      h ^= (unsigned char)*p;
      h *= 16777619u;
   }
   snprintf(out, cap, "%08x", h);
   return 0;
}

/* Check if a path is already inside an aimee worktree. */
int is_aimee_worktree_path(const char *path)
{
   return path && (strstr(path, "/.aimee/worktrees/") != NULL || strstr(path, "/.aimee-") != NULL);
}

int worktree_managed_git_root(const char *path, char *out, size_t out_len)
{
   if (!path || !out || out_len == 0)
      return -1;
   out[0] = '\0';
   const char *marker = strstr(path, "/.aimee/worktrees/");
   if (!marker || marker == path)
      return -1;
   size_t n = (size_t)(marker - path);
   if (n >= out_len)
      return -1;
   memcpy(out, path, n);
   out[n] = '\0';
   return out[0] ? 0 : -1;
}

/* Count active aimee worktrees for a given git_root. */
int count_active_worktrees_for_root(const char *git_root)
{
   if (!git_root)
      return 0;

   int count = 0;

   char managed_root[MAX_PATH_LEN];
   snprintf(managed_root, sizeof(managed_root), "%s/.aimee/worktrees", git_root);
   DIR *managed = opendir(managed_root);
   if (managed)
   {
      struct dirent *sid_ent;
      while ((sid_ent = readdir(managed)) != NULL)
      {
         if (sid_ent->d_name[0] == '.')
            continue;

         char sid_path[MAX_PATH_LEN];
         snprintf(sid_path, sizeof(sid_path), "%s/%s", managed_root, sid_ent->d_name);
         DIR *sid_dir = opendir(sid_path);
         if (!sid_dir)
            continue;

         struct dirent *work_ent;
         while ((work_ent = readdir(sid_dir)) != NULL)
         {
            if (work_ent->d_name[0] == '.')
               continue;
            char wt_path[MAX_PATH_LEN];
            snprintf(wt_path, sizeof(wt_path), "%s/%s", sid_path, work_ent->d_name);
            struct stat st;
            if (stat(wt_path, &st) == 0 && S_ISDIR(st.st_mode))
               count++;
         }
         closedir(sid_dir);
      }
      closedir(managed);
   }

   /* Backward compatibility while old out-of-repo worktrees still exist. */
   char parent[MAX_PATH_LEN];
   snprintf(parent, sizeof(parent), "%s", git_root);
   char *slash = strrchr(parent, '/');
   if (!slash || slash == parent)
      return count;
   *slash = '\0';
   const char *basename = slash + 1;

   char prefix[MAX_PATH_LEN];
   snprintf(prefix, sizeof(prefix), ".aimee-%s-", basename);
   size_t prefix_len = strlen(prefix);

   DIR *d = opendir(parent);
   if (!d)
      return count;

   struct dirent *ent;
   while ((ent = readdir(d)) != NULL)
   {
      if (strncmp(ent->d_name, prefix, prefix_len) == 0)
         count++;
   }
   closedir(d);
   return count;
}

static void worktree_registry_paths(const char *git_root, char *repo_path, size_t repo_len,
                                    char *global_path, size_t global_len)
{
   if (repo_path && repo_len > 0)
      snprintf(repo_path, repo_len, "%s/.aimee/worktrees/registry.tsv", git_root);
   if (global_path && global_len > 0)
      snprintf(global_path, global_len, "%s/worktrees.tsv", config_output_dir());
}

static void worktree_registry_append(const char *path, const char *git_root, const char *wt_path,
                                     const char *branch, const char *sid, const char *work_name)
{
   if (!path || !path[0])
      return;

   char parent[MAX_PATH_LEN];
   snprintf(parent, sizeof(parent), "%s", path);
   char *slash = strrchr(parent, '/');
   if (slash)
   {
      *slash = '\0';
      platform_mkdir_p(parent, 0755);
   }

   FILE *f = fopen(path, "a");
   if (!f)
      return;
   fprintf(f, "%s\t%s\t%s\t%s\t%s\n", git_root ? git_root : "", wt_path ? wt_path : "",
           branch ? branch : "", sid ? sid : "", work_name ? work_name : "");
   fclose(f);
}

void worktree_registry_record(const char *git_root, const char *wt_path, const char *branch,
                              const char *sid, const char *work_name)
{
   if (!git_root || !git_root[0] || !wt_path || !wt_path[0])
      return;

   char repo_path[MAX_PATH_LEN], global_path[MAX_PATH_LEN];
   worktree_registry_paths(git_root, repo_path, sizeof(repo_path), global_path,
                           sizeof(global_path));
   worktree_registry_append(repo_path, git_root, wt_path, branch, sid, work_name);
   worktree_registry_append(global_path, git_root, wt_path, branch, sid, work_name);
}

static void worktree_registry_remove_from_file(const char *path, const char *wt_path)
{
   if (!path || !path[0] || !wt_path || !wt_path[0])
      return;

   FILE *in = fopen(path, "r");
   if (!in)
      return;

   char tmp_path[MAX_PATH_LEN];
   snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
   FILE *out = fopen(tmp_path, "w");
   if (!out)
   {
      fclose(in);
      return;
   }

   char line[MAX_PATH_LEN * 3];
   while (fgets(line, sizeof(line), in))
   {
      char copy[MAX_PATH_LEN * 3];
      snprintf(copy, sizeof(copy), "%s", line);
      char *save = NULL;
      (void)strtok_r(copy, "\t\n", &save); /* git_root */
      char *path_field = strtok_r(NULL, "\t\n", &save);
      if (path_field && strcmp(path_field, wt_path) == 0)
         continue;
      fputs(line, out);
   }

   fclose(in);
   fclose(out);
   rename(tmp_path, path);
}

static void worktree_registry_remove(const char *git_root, const char *wt_path)
{
   char repo_path[MAX_PATH_LEN], global_path[MAX_PATH_LEN];
   worktree_registry_paths(git_root, repo_path, sizeof(repo_path), global_path,
                           sizeof(global_path));
   worktree_registry_remove_from_file(repo_path, wt_path);
   worktree_registry_remove_from_file(global_path, wt_path);
}

int worktree_find_branch_in_repo(const char *git_root, const char *branch, char *out_dir,
                                 size_t out_len)
{
   if (!git_root || !git_root[0] || !branch || !branch[0] || !out_dir || out_len == 0)
      return 0;

   char cmd[MAX_PATH_LEN + 128];
   snprintf(cmd, sizeof(cmd), "git -C '%s' worktree list --porcelain 2>/dev/null", git_root);

   int rc;
   char *out = run_cmd(cmd, &rc);
   if (rc != 0 || !out)
   {
      free(out);
      return 0;
   }

   char want_ref[320];
   snprintf(want_ref, sizeof(want_ref), "refs/heads/%s", branch);

   char current_worktree[MAX_PATH_LEN] = "";
   char *save = NULL;
   int found = 0;
   for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
   {
      if (strncmp(line, "worktree ", 9) == 0)
      {
         snprintf(current_worktree, sizeof(current_worktree), "%s", line + 9);
      }
      else if (strncmp(line, "branch ", 7) == 0)
      {
         const char *ref = line + 7;
         if (strcmp(ref, want_ref) == 0 || strcmp(ref, branch) == 0)
         {
            snprintf(out_dir, out_len, "%s", current_worktree);
            found = out_dir[0] != '\0';
            break;
         }
      }
   }

   free(out);
   return found;
}

static int worktree_path_on_branch(const char *wt_path, const char *branch)
{
   char cmd[MAX_PATH_LEN + 128];
   snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse --abbrev-ref HEAD 2>/dev/null", wt_path);
   int rc;
   char *out = run_cmd(cmd, &rc);
   if (rc != 0 || !out)
   {
      free(out);
      return 0;
   }
   char *nl = strchr(out, '\n');
   if (nl)
      *nl = '\0';
   int match = strcmp(out, branch) == 0;
   free(out);
   return match;
}

int worktree_find_branch_registered(const char *branch, char *out_dir, size_t out_len)
{
   if (!branch || !branch[0] || !out_dir || out_len == 0)
      return 0;

   char global_path[MAX_PATH_LEN];
   worktree_registry_paths("", NULL, 0, global_path, sizeof(global_path));
   FILE *f = fopen(global_path, "r");
   if (!f)
      return 0;

   int found = 0;
   char line[MAX_PATH_LEN * 3];
   while (fgets(line, sizeof(line), f))
   {
      char *save = NULL;
      char *git_root = strtok_r(line, "\t\n", &save);
      char *wt_path = strtok_r(NULL, "\t\n", &save);
      char *recorded_branch = strtok_r(NULL, "\t\n", &save);
      if (!git_root || !wt_path)
         continue;

      if (recorded_branch && strcmp(recorded_branch, branch) == 0 &&
          worktree_path_on_branch(wt_path, branch))
      {
         snprintf(out_dir, out_len, "%s", wt_path);
         found = 1;
      }
      else
      {
         char branch_dir[MAX_PATH_LEN];
         if (worktree_find_branch_in_repo(git_root, branch, branch_dir, sizeof(branch_dir)))
         {
            snprintf(out_dir, out_len, "%s", branch_dir);
            found = 1;
         }
      }
   }

   fclose(f);
   return found;
}

/* Detect the best base branch for a new worktree rooted at git_root.
 * Prefers the current checked-out branch; falls back to main/origin/main/HEAD. */
void worktree_detect_base_branch(const char *git_root, char *buf, size_t buf_len)
{
   if (!git_root || !buf || buf_len == 0)
      return;
   snprintf(buf, buf_len, "HEAD"); /* safe default */

   int found = 0;
   char cmd[MAX_PATH_LEN + 128];
   int rc;

   snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse --abbrev-ref HEAD 2>/dev/null", git_root);
   char *out = run_cmd(cmd, &rc);
   if (rc == 0 && out && out[0])
   {
      /* Strip trailing newline */
      size_t len = strlen(out);
      while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
         out[--len] = '\0';
      /* Use if it's a real branch, not a detached HEAD */
      if (len > 0 && strcmp(out, "HEAD") != 0)
      {
         snprintf(buf, buf_len, "%s", out);
         found = 1;
      }
   }
   free(out);

   if (!found)
   {
      const char *candidates[] = {"main", "origin/main", "HEAD"};
      for (int b = 0; b < 3; b++)
      {
         snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse --verify '%s' 2>/dev/null", git_root,
                  candidates[b]);
         char *cand_out = run_cmd(cmd, &rc);
         free(cand_out);
         if (rc == 0)
         {
            snprintf(buf, buf_len, "%s", candidates[b]);
            break;
         }
      }
   }
}

static int worktree_create_sibling_at_ref(const char *git_root, const char *sid,
                                          const char *work_name, const char *base_ref)
{
   if (!git_root || !sid)
      return -1;

   char wt_path[MAX_PATH_LEN];
   if (worktree_sibling_path(git_root, sid, work_name, wt_path, sizeof(wt_path)) != 0)
      return -1;

   /* Create branch name from session ID (and optional work name) */
   char short_id[12];
   snprintf(short_id, sizeof(short_id), "%.8s", sid);
   char branch_name[128];
   if (work_name && work_name[0])
      snprintf(branch_name, sizeof(branch_name), "aimee/session/%s/%s", short_id, work_name);
   else
      snprintf(branch_name, sizeof(branch_name), "aimee/session/%s", short_id);

   /* Check if worktree already exists and is valid */
   struct stat st;
   if (stat(wt_path, &st) == 0 && S_ISDIR(st.st_mode))
   {
      char git_file[MAX_PATH_LEN];
      snprintf(git_file, sizeof(git_file), "%s/.git", wt_path);
      struct stat git_st;
      if (stat(git_file, &git_st) == 0)
      {
         worktree_registry_record(git_root, wt_path, branch_name, sid, work_name);
         return 0; /* already exists and valid */
      }

      /* Directory exists but is not a git worktree; remove the stale dir. */
      LOG_WARN("workspace", "worktree dir '%s' exists but is not a git worktree - removing",
               wt_path);
      rmdir(wt_path);
   }

   char base_branch[64];
   if (base_ref && base_ref[0])
      snprintf(base_branch, sizeof(base_branch), "%s", base_ref);
   else
   {
      /* Detect base branch: prefer the workspace's current branch so worktrees
       * are rooted there rather than always on main. Falls back to main /
       * origin/main / HEAD when in a detached-HEAD state or when the lookup
       * fails (e.g. empty repo). */
      worktree_detect_base_branch(git_root, base_branch, sizeof(base_branch));
   }

   char wt_parent[MAX_PATH_LEN];
   snprintf(wt_parent, sizeof(wt_parent), "%s", wt_path);
   char *slash = strrchr(wt_parent, '/');
   if (slash)
   {
      *slash = '\0';
      platform_mkdir_p(wt_parent, 0755);
   }

   /* Create the worktree */
   char cmd[MAX_PATH_LEN * 2 + 256];
   snprintf(cmd, sizeof(cmd), "git -C '%s' worktree add '%s' -b '%s' '%s' 2>&1", git_root, wt_path,
            branch_name, base_branch);
   int rc;
   char *out = run_cmd(cmd, &rc);

   if (rc == 0)
   {
      fprintf(stderr, "aimee: created worktree at %s\n", wt_path);
      free(out);
      worktree_registry_record(git_root, wt_path, branch_name, sid, work_name);

      /* Register branch ownership so other sessions can't write to it.
       * Use the main repo root (git_root), not the worktree path. */
#ifndef AIMEE_DB1_DISABLED
      mcp_git_branch_own_register(git_root, branch_name);
#endif

      return 0;
   }

   fprintf(stderr, "aimee: failed to create worktree at %s: %s\n", wt_path, out ? out : "unknown");
   free(out);
   return -1;
}

/* Replay the anchor worktree's uncommitted changes (tracked modifications +
 * deletions, plus untracked non-ignored files) into the freshly created
 * worktree at wt_path, so a delegate starts from the parent's CURRENT
 * working-tree state rather than the parent's last commit. The caller bases the
 * new worktree on the anchor's HEAD, so `git diff HEAD` from the anchor applies
 * cleanly onto it. Best-effort: logs and continues on partial failure (the
 * delegate still has the committed base). Exposed (non-static) for unit tests;
 * declared in workspace.h. */
void worktree_apply_anchor_wip(const char *anchor_dir, const char *wt_path)
{
   if (!anchor_dir || !anchor_dir[0] || !wt_path || !wt_path[0])
      return;
   if (strcmp(anchor_dir, wt_path) == 0)
      return;

   char cmd[MAX_PATH_LEN * 4 + 512];
   int rc = 0;
   char *out = NULL;

   /* 1. Tracked changes (staged + unstaged + deletions, including binary). The
    *    patch is generated relative to HEAD and applied to the working tree of
    *    the new worktree, which sits at the same commit. */
   /* Only replay when the anchor actually has tracked changes. An empty diff
    * makes `git apply` fail with "No valid patches in input" -- not a real
    * error, but it produced a spurious WARN on every clean-parent spawn. */
   int has_changes = 0;
   snprintf(cmd, sizeof(cmd), "git -C '%s' diff HEAD --quiet 2>/dev/null", anchor_dir);
   free(run_cmd(cmd, &has_changes)); /* `git diff --quiet` exits 1 iff changes exist */
   if (has_changes != 0)
   {
      snprintf(cmd, sizeof(cmd),
               "git -C '%s' diff HEAD --binary --no-color 2>/dev/null | "
               "git -C '%s' apply --whitespace=nowarn - 2>&1",
               anchor_dir, wt_path);
      out = run_cmd(cmd, &rc);
      if (rc != 0 && out && out[0])
         LOG_WARN("workspace",
                  "delegate worktree '%s': could not replay anchor tracked changes: %s", wt_path,
                  out);
      free(out);
   }

   /* 2. Untracked, non-ignored files: copy each from the anchor, recreating its
    *    parent directories. Portable POSIX-sh loop (run_cmd uses /bin/sh, which
    *    may be dash — no `read -d`/bashisms); quoting tolerates spaces in paths. */
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git ls-files --others --exclude-standard 2>/dev/null | "
            "while IFS= read -r f; do [ -n \"$f\" ] || continue; "
            "mkdir -p '%s/'\"$(dirname \"$f\")\" && cp -a \"$f\" '%s/'\"$f\"; done 2>&1",
            anchor_dir, wt_path, wt_path);
   rc = 0;
   out = run_cmd(cmd, &rc);
   if (rc != 0 && out && out[0])
      LOG_WARN("workspace", "delegate worktree '%s': could not copy anchor untracked files: %s",
               wt_path, out);
   free(out);
}

/* Create an aimee-managed worktree. Returns 0 on success, -1 on failure.
 * If work_name is non-NULL, creates a separate worktree for that work unit. */
int worktree_create_sibling(const char *git_root, const char *sid, const char *work_name)
{
   return worktree_create_sibling_at_ref(git_root, sid, work_name, NULL);
}

int worktree_create_sibling_from_ref(const char *git_root, const char *sid, const char *work_name,
                                     const char *base_ref)
{
   return worktree_create_sibling_at_ref(git_root, sid, work_name, base_ref);
}

int worktree_create_sibling_from_anchor(const char *git_root, const char *sid,
                                        const char *work_name, const char *anchor_dir)
{
   char base_ref[64] = "";
   if (anchor_dir && anchor_dir[0])
   {
      char cmd[MAX_PATH_LEN + 128];
      snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse --verify HEAD 2>/dev/null", anchor_dir);
      int rc;
      char *out = run_cmd(cmd, &rc);
      if (rc == 0 && out && out[0])
      {
         size_t n = strlen(out);
         while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
            out[--n] = '\0';
         if (n > 0 && n < sizeof(base_ref))
            snprintf(base_ref, sizeof(base_ref), "%s", out);
      }
      free(out);
   }
   int rc = worktree_create_sibling_at_ref(git_root, sid, work_name, base_ref[0] ? base_ref : NULL);
   /* Carry the anchor's uncommitted working-tree state into the new worktree so
    * the delegate is grounded in the parent's CURRENT files, not its last
    * commit. Only when we based on the anchor's HEAD (base_ref derived above). */
   if (rc == 0 && base_ref[0] && anchor_dir && anchor_dir[0])
   {
      char wt_path[MAX_PATH_LEN];
      if (worktree_sibling_path(git_root, sid, work_name, wt_path, sizeof(wt_path)) == 0)
         worktree_apply_anchor_wip(anchor_dir, wt_path);
   }
   return rc;
}

/* Create a worktree targeting a named branch.
 * When the branch exists and is not checked out elsewhere, the worktree is
 * placed directly on it via 'git worktree add <path> <branch>'.  When that
 * fails (branch checked out in another worktree, or doesn't exist yet),
 * falls back to an auto-named session branch so the delegate can still
 * commit and the apply-back step copies file changes to the parent. */
int worktree_create_sibling_on_branch(const char *git_root, const char *sid, const char *work_name,
                                      const char *branch, const char *anchor_dir)
{
   if (!branch || !branch[0])
      return worktree_create_sibling_from_anchor(git_root, sid, work_name, anchor_dir);

   char wt_path[MAX_PATH_LEN];
   if (worktree_sibling_path(git_root, sid, work_name, wt_path, sizeof(wt_path)) != 0)
      return -1;

   struct stat st;
   if (stat(wt_path, &st) == 0 && S_ISDIR(st.st_mode))
   {
      char git_file[MAX_PATH_LEN];
      snprintf(git_file, sizeof(git_file), "%s/.git", wt_path);
      struct stat git_st;
      if (stat(git_file, &git_st) == 0)
      {
         worktree_registry_record(git_root, wt_path, branch, sid, work_name);
         return 0;
      }
      rmdir(wt_path);
   }

   char wt_parent[MAX_PATH_LEN];
   snprintf(wt_parent, sizeof(wt_parent), "%s", wt_path);
   char *slash = strrchr(wt_parent, '/');
   if (slash)
   {
      *slash = '\0';
      platform_mkdir_p(wt_parent, 0755);
   }

   char cmd[MAX_PATH_LEN * 2 + 256];
   int rc;
   char *out;

   snprintf(cmd, sizeof(cmd), "git -C '%s' worktree add '%s' '%s' 2>&1", git_root, wt_path, branch);
   out = run_cmd(cmd, &rc);
   if (rc == 0)
   {
      fprintf(stderr, "aimee: created worktree at %s on branch %s\n", wt_path, branch);
      free(out);
      worktree_registry_record(git_root, wt_path, branch, sid, work_name);
#ifndef AIMEE_DB1_DISABLED
      mcp_git_branch_own_register(git_root, branch);
#endif
      /* Carry the anchor's uncommitted WIP so the delegate sees the parent's
       * CURRENT files, not just the branch tip. Best-effort: the tracked patch
       * applies cleanly only when the checked-out branch sits at the anchor's
       * HEAD (the common case) and no-ops otherwise; untracked files copy
       * regardless. */
      if (anchor_dir && anchor_dir[0])
         worktree_apply_anchor_wip(anchor_dir, wt_path);
      return 0;
   }
   free(out);

   /* Fall back: branch unavailable for direct checkout; use auto-named branch */
   return worktree_create_sibling_from_anchor(git_root, sid, work_name, anchor_dir);
}

/* Clean up a session's worktree. Removes if clean, warns if dirty.
 * If work_name is non-NULL, targets the work-specific worktree. */
void worktree_cleanup(const char *git_root, const char *sid, const char *work_name)
{
   if (!git_root || !sid)
      return;

   char wt_path[MAX_PATH_LEN];
   if (worktree_sibling_path(git_root, sid, work_name, wt_path, sizeof(wt_path)) != 0)
      return;

   struct stat st;
   if (stat(wt_path, &st) != 0)
      return; /* doesn't exist */

   /* Check for uncommitted changes */
   char cmd[MAX_PATH_LEN + 128];
   snprintf(cmd, sizeof(cmd), "git -C '%s' status --porcelain 2>/dev/null", wt_path);
   int rc;
   char *status = run_cmd(cmd, &rc);
   int has_changes = (status && status[0]);
   free(status);

   /* Check for unpushed commits */
   int has_unpushed = 0;
   if (!has_changes)
   {
      snprintf(cmd, sizeof(cmd), "git -C '%s' log @{upstream}..HEAD --oneline 2>/dev/null",
               wt_path);
      char *log = run_cmd(cmd, &rc);
      has_unpushed = (log && log[0]);
      free(log);
   }

   if (has_changes || has_unpushed)
   {
      LOG_WARN("workspace", "worktree %s has %s - not removing", wt_path,
               has_changes ? "uncommitted changes" : "unpushed commits");
      return;
   }

   /* Clean; remove the worktree. */
   snprintf(cmd, sizeof(cmd), "git -C '%s' worktree remove '%s' 2>&1", git_root, wt_path);
   char *out = run_cmd(cmd, &rc);
   if (rc == 0)
   {
      fprintf(stderr, "aimee: removed clean worktree %s\n", wt_path);
      worktree_registry_remove(git_root, wt_path);
   }
   else
      fprintf(stderr, "aimee: failed to remove worktree %s: %s\n", wt_path, out ? out : "unknown");
   free(out);
}

static char *worktree_status_porcelain(const char *wt_path)
{
   if (!wt_path || !wt_path[0])
      return NULL;

   char *esc = shell_escape(wt_path);
   if (!esc)
      return NULL;
   char cmd[MAX_PATH_LEN * 2 + 128];
   snprintf(cmd, sizeof(cmd), "git -C '%s' status --porcelain 2>/dev/null", esc);
   free(esc);

   int rc;
   char *status = run_cmd(cmd, &rc);
   if (rc != 0)
   {
      free(status);
      return NULL;
   }
   return status;
}

static int worktree_path_has_changes(const char *wt_path, const char *rel_path)
{
   if (!wt_path || !wt_path[0] || !rel_path || !rel_path[0])
      return 0;

   char *esc_wt = shell_escape(wt_path);
   char *esc_path = shell_escape(rel_path);
   if (!esc_wt || !esc_path)
   {
      free(esc_wt);
      free(esc_path);
      return 0;
   }

   char cmd[MAX_PATH_LEN * 3 + 128];
   snprintf(cmd, sizeof(cmd), "git -C '%s' status --porcelain -- '%s' 2>/dev/null", esc_wt,
            esc_path);
   free(esc_wt);
   free(esc_path);

   int rc;
   char *status = run_cmd(cmd, &rc);
   int dirty = (rc == 0 && status && status[0]);
   free(status);
   return dirty;
}

static int worktree_parse_status_line(const char *line, char *old_path, size_t old_len,
                                      char *new_path, size_t new_len, int *is_delete)
{
   if (!line || strlen(line) < 4 || !new_path || new_len == 0 || !is_delete)
      return -1;

   char x = line[0];
   char y = line[1];
   const char *path = line + 3;
   const char *arrow = strstr(path, " -> ");
   if (old_path && old_len > 0)
      old_path[0] = '\0';

   if (arrow)
   {
      if (old_path && old_len > 0)
      {
         size_t n = (size_t)(arrow - path);
         if (n >= old_len)
            n = old_len - 1;
         memcpy(old_path, path, n);
         old_path[n] = '\0';
      }
      snprintf(new_path, new_len, "%s", arrow + 4);
      *is_delete = 0;
   }
   else
   {
      snprintf(new_path, new_len, "%s", path);
      *is_delete = (x == 'D' || y == 'D');
   }

   return new_path[0] ? 0 : -1;
}

static int workspace_mkdir_parent(const char *path)
{
   if (!path || !path[0])
      return -1;
   char parent[MAX_PATH_LEN * 2];
   snprintf(parent, sizeof(parent), "%s", path);
   char *slash = strrchr(parent, '/');
   if (!slash)
      return 0;
   *slash = '\0';
   return platform_mkdir_p(parent, 0755);
}

static int workspace_copy_file(const char *src_path, const char *dst_path)
{
   FILE *in = fopen(src_path, "rb");
   if (!in)
      return -1;
   if (workspace_mkdir_parent(dst_path) != 0)
   {
      fclose(in);
      return -1;
   }
   FILE *out = fopen(dst_path, "wb");
   if (!out)
   {
      fclose(in);
      return -1;
   }

   char buf[16384];
   int rc = 0;
   for (;;)
   {
      size_t n = fread(buf, 1, sizeof(buf), in);
      if (n > 0 && fwrite(buf, 1, n, out) != n)
      {
         rc = -1;
         break;
      }
      if (n < sizeof(buf))
      {
         if (ferror(in))
            rc = -1;
         break;
      }
   }

   struct stat st;
   if (rc == 0 && stat(src_path, &st) == 0)
      (void)chmod(dst_path, st.st_mode & 0777);

   if (fclose(out) != 0)
      rc = -1;
   fclose(in);
   return rc;
}

/* Copy uncommitted changes from src_wt into dst_wt.
 * Reads git status --porcelain from src_wt and applies each modified, added,
 * untracked, renamed, or deleted path to the corresponding path under dst_wt.
 * Returns count of files applied, 0 if no changes, -1 on error. */
int worktree_apply_changes_to_parent(const char *src_wt, const char *dst_wt)
{
   if (!src_wt || !dst_wt || !src_wt[0] || !dst_wt[0])
      return -1;

   char *status = worktree_status_porcelain(src_wt);
   if (!status)
      return -1;
   if (!status[0])
   {
      free(status);
      return 0;
   }

   int count = 0;
   char *line = status;
   while (*line)
   {
      char *eol = strchr(line, '\n');
      if (eol)
         *eol = '\0';

      if (strlen(line) >= 4)
      {
         char old_path[MAX_PATH_LEN * 2];
         char rel_path[MAX_PATH_LEN * 2];
         int is_delete = 0;
         if (worktree_parse_status_line(line, old_path, sizeof(old_path), rel_path,
                                        sizeof(rel_path), &is_delete) == 0)
         {
            if (old_path[0])
            {
               char dst_old_path[MAX_PATH_LEN * 2];
               snprintf(dst_old_path, sizeof(dst_old_path), "%s/%s", dst_wt, old_path);
               if (unlink(dst_old_path) == 0)
                  count++;
            }

            char src_path[MAX_PATH_LEN * 2];
            char dst_path[MAX_PATH_LEN * 2];
            snprintf(src_path, sizeof(src_path), "%s/%s", src_wt, rel_path);
            snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_wt, rel_path);

            if (is_delete)
            {
               if (unlink(dst_path) == 0 || errno == ENOENT)
                  count++;
            }
            else if (workspace_copy_file(src_path, dst_path) == 0)
            {
               count++;
            }
         }
      }

      line = eol ? eol + 1 : line + strlen(line);
   }

   free(status);
   return count;
}

/* Byte-for-byte file comparison. Returns 1 only if both files open and have
 * identical contents; 0 on any difference or open failure. */
static int files_byte_identical(const char *a, const char *b)
{
   FILE *fa = fopen(a, "rb");
   FILE *fb = fopen(b, "rb");
   if (!fa || !fb)
   {
      if (fa)
         fclose(fa);
      if (fb)
         fclose(fb);
      return 0;
   }
   int same = 1;
   for (;;)
   {
      int ca = fgetc(fa);
      int cb = fgetc(fb);
      if (ca != cb)
      {
         same = 0;
         break;
      }
      if (ca == EOF)
         break;
   }
   fclose(fa);
   fclose(fb);
   return same;
}

int worktree_apply_delegate_changes_to_parent(const char *delegate_wt, const char *parent_wt,
                                              char *err, size_t err_len)
{
   if (err && err_len > 0)
      err[0] = '\0';
   if (!delegate_wt || !parent_wt || !delegate_wt[0] || !parent_wt[0])
   {
      if (err && err_len > 0)
         snprintf(err, err_len, "missing delegate or parent worktree path");
      return -1;
   }

   char *status = worktree_status_porcelain(delegate_wt);
   if (!status)
   {
      if (err && err_len > 0)
         snprintf(err, err_len, "failed to read delegate worktree status");
      return -1;
   }
   if (!status[0])
   {
      free(status);
      return 0;
   }

   char *line = status;
   while (*line)
   {
      char *eol = strchr(line, '\n');
      if (eol)
         *eol = '\0';

      char old_path[MAX_PATH_LEN * 2];
      char rel_path[MAX_PATH_LEN * 2];
      int is_delete = 0;
      if (worktree_parse_status_line(line, old_path, sizeof(old_path), rel_path, sizeof(rel_path),
                                     &is_delete) == 0)
      {
         /* The WIP-carry copies the parent's uncommitted files into the delegate
          * worktree, so they reappear in `git status` here even though the
          * delegate never touched them. Re-applying such a file collides with
          * the parent's own copy and aborts the whole apply-back, losing the
          * delegate's real work. A file whose delegate content is byte-identical
          * to the parent's current content is carried WIP, not a delegate change
          * -- skip the conflict check for it (copying it back is a harmless
          * no-op). A genuine conflict (delegate and parent changed the same file
          * differently) still differs in content and is still refused. */
         int carried = 0;
         if (!is_delete && rel_path[0])
         {
            char dpath[MAX_PATH_LEN * 2 + 2], ppath[MAX_PATH_LEN * 2 + 2];
            snprintf(dpath, sizeof(dpath), "%s/%s", delegate_wt, rel_path);
            snprintf(ppath, sizeof(ppath), "%s/%s", parent_wt, rel_path);
            carried = files_byte_identical(dpath, ppath);
         }
         if (!carried && ((old_path[0] && worktree_path_has_changes(parent_wt, old_path)) ||
                          worktree_path_has_changes(parent_wt, rel_path)))
         {
            if (err && err_len > 0)
               snprintf(err, err_len,
                        "refusing to apply delegate changes over existing parent change: %s",
                        old_path[0] ? old_path : rel_path);
            free(status);
            return -1;
         }
      }

      line = eol ? eol + 1 : line + strlen(line);
   }
   free(status);

   int applied = worktree_apply_changes_to_parent(delegate_wt, parent_wt);
   if (applied < 0 && err && err_len > 0)
      snprintf(err, err_len, "failed to copy delegate changes into parent worktree");
   return applied;
}

int worktree_apply_delegate_changes_checked(const char *delegate_wt, const char *parent_hint,
                                            const char *launch_head, int *applied,
                                            char *parent_root, size_t parent_root_len, char *err,
                                            size_t err_len)
{
   char root[MAX_PATH_LEN] = "";
   if (applied)
      *applied = -1;
   if (err && err_len > 0)
      err[0] = '\0';
   if (parent_root && parent_root_len > 0)
      parent_root[0] = '\0';
   /* Resolve the parent root to the worktree that actually launched the
    * delegate, not the main repository. git_repo_root() collapses a linked
    * worktree to the main checkout (parent of the common .git dir); using it
    * here would (a) compare launch_head against the wrong worktree's HEAD,
    * producing a false "parent HEAD changed" refusal, and (b) apply delegate
    * changes into the main checkout instead of the session worktree. The
    * delegate's launch_head is captured against the session worktree, so the
    * apply target must be the same worktree. --show-toplevel resolves to the
    * current worktree (identical to git_repo_root in a non-worktree checkout). */
   if (parent_hint && parent_hint[0])
   {
      char top_cmd[MAX_PATH_LEN + 96];
      int top_rc = 0;
      snprintf(top_cmd, sizeof(top_cmd), "git -C '%s' rev-parse --show-toplevel 2>/dev/null",
               parent_hint);
      char *top = run_cmd(top_cmd, &top_rc);
      if (top_rc == 0 && top && top[0])
      {
         top[strcspn(top, "\r\n")] = '\0';
         snprintf(root, sizeof(root), "%s", top);
      }
      else
      {
         snprintf(root, sizeof(root), "%s", parent_hint);
      }
      free(top);
   }
   if (!root[0])
   {
      snprintf(err, err_len, "cannot apply changes; parent worktree root is unavailable");
      return -1;
   }
   if (parent_root && parent_root_len > 0)
      snprintf(parent_root, parent_root_len, "%s", root);
   if (launch_head && launch_head[0])
   {
      char cmd[MAX_PATH_LEN + 96];
      int rc = 0;
      snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse --verify HEAD 2>/dev/null", root);
      char *head = run_cmd(cmd, &rc);
      if (rc != 0 || !head)
      {
         free(head);
         snprintf(err, err_len, "cannot verify parent HEAD before applying delegate changes");
         return -1;
      }
      head[strcspn(head, "\r\n")] = '\0';
      int changed = strcmp(head, launch_head) != 0;
      free(head);
      if (changed)
      {
         snprintf(err, err_len,
                  "refusing to apply changes because parent HEAD changed during delegation");
         return -1;
      }
   }
   int n = worktree_apply_delegate_changes_to_parent(delegate_wt, root, err, err_len);
   if (n < 0)
   {
      if (err && err_len > 0 && !err[0])
         snprintf(err, err_len, "failed to apply changes to parent worktree");
      return -1;
   }
   if (applied)
      *applied = n;
   return 0;
}

/* Decide whether the session needs to switch into its per-session
 * worktree. A new top-level session must not reuse a previous session's
 * managed worktree: only the expected worktree for this same sid counts as
 * already isolated. Returns 1 and fills target with the worktree path when a
 * switch is required; 0 otherwise. */
int session_isolation_target(const char *cwd, const char *sid, char *target, size_t target_len,
                             int create_if_missing)
{
#ifdef AIMEE_DB1_DISABLED
   (void)cwd;
   (void)sid;
   (void)target;
   (void)target_len;
   (void)create_if_missing;
   return 0;
#else
   if (!cwd || !sid || !sid[0] || !target || target_len == 0)
      return 0;
   target[0] = '\0';

   char gr[MAX_PATH_LEN];
   if (git_repo_root(cwd, gr, sizeof(gr)) != 0)
      return 0;

   /* Defensive: a future git_repo_root that resolves to the worktree itself
    * should still be treated as isolated. */
   if (is_aimee_worktree_path(gr))
      return 0;

   char wt_path[MAX_PATH_LEN];
   if (worktree_sibling_path(gr, sid, NULL, wt_path, sizeof(wt_path)) != 0)
      return 0;

   if (is_aimee_worktree_path(cwd))
   {
      size_t wlen = strlen(wt_path);
      if (strncmp(cwd, wt_path, wlen) == 0 && (cwd[wlen] == '/' || cwd[wlen] == '\0'))
         return 0;
   }

   struct stat wst;
   if (stat(wt_path, &wst) != 0)
   {
      if (!create_if_missing)
         return 0;
      if (worktree_create_sibling(gr, sid, NULL) != 0)
         return 0;
      if (stat(wt_path, &wst) != 0)
         return 0;
   }

   snprintf(target, target_len, "%s", wt_path);
   return 1;
#endif
}

/* Check if the current branch has a merged PR. Returns 1 if merged. */
int check_merged_pr_for_branch(const char *git_dir)
{
   int rc;
   char cmd_buf[MAX_PATH_LEN + 128];
   if (git_dir && git_dir[0])
      snprintf(cmd_buf, sizeof(cmd_buf), "git -C '%s' rev-parse --abbrev-ref HEAD 2>/dev/null",
               git_dir);
   else
      snprintf(cmd_buf, sizeof(cmd_buf), "git rev-parse --abbrev-ref HEAD 2>/dev/null");
   char *branch = run_cmd(cmd_buf, &rc);
   if (rc != 0 || !branch)
   {
      free(branch);
      return 0;
   }
   char *nl = strchr(branch, '\n');
   if (nl)
      *nl = '\0';

   /* Skip default branches */
   if (strcmp(branch, "main") == 0 || strcmp(branch, "master") == 0)
   {
      free(branch);
      return 0;
   }

   /* Run `gh` inside the target repo. Without `cd`, `gh` inherits aimee-server's
    * cwd (the aimee repo) and queries the wrong GitHub repo — producing
    * false-positive merged-PR hits when a branch name collides across repos
    * (e.g. `aimee/session/<id>` exists as a merged aimee PR while being the
    * current session branch in an unrelated checkout). */
   char cmd[MAX_PATH_LEN + 256];
   if (git_dir && git_dir[0])
      snprintf(cmd, sizeof(cmd),
               "cd '%s' && gh pr list --head '%s' --state merged --json number --limit 1 "
               "2>/dev/null",
               git_dir, branch);
   else
      snprintf(cmd, sizeof(cmd),
               "gh pr list --head '%s' --state merged --json number --limit 1 2>/dev/null", branch);
   free(branch);

   char *out = run_cmd(cmd, &rc);
   if (rc != 0 || !out)
   {
      free(out);
      return 0;
   }

   int has_merged = (strstr(out, "\"number\"") != NULL);
   free(out);
   if (!has_merged)
      return 0;

   /* A branch can have a merged PR and still be an active staging branch
    * (e.g. proposal/next-cycle accumulates PRs and then merges to main
    * repeatedly). Allow push when HEAD is ahead of origin/main — the branch
    * has new commits that were not part of the merged PR. */
   char ahead_cmd[MAX_PATH_LEN + 256];
   if (git_dir && git_dir[0])
      snprintf(ahead_cmd, sizeof(ahead_cmd),
               "cd '%s' && git rev-list --count origin/main..HEAD 2>/dev/null", git_dir);
   else
      snprintf(ahead_cmd, sizeof(ahead_cmd), "git rev-list --count origin/main..HEAD 2>/dev/null");
   char *ahead = run_cmd(ahead_cmd, &rc);
   if (ahead)
   {
      int n = atoi(ahead);
      free(ahead);
      if (n > 0)
         return 0;
   }
   return 1;
}

/* Look up the worktree path for a given CWD from session state.
 * Returns the worktree path if the CWD is inside a tracked git root. */
const char *worktree_for_cwd(const session_state_t *state, const char *cwd)
{
   if (!state || !cwd || state->worktree_count == 0)
      return NULL;

   /* Find the most specific (longest) matching git root */
   int best = -1;
   size_t best_len = 0;
   for (int i = 0; i < state->worktree_count; i++)
   {
      size_t rlen = strlen(state->worktrees[i].git_root);
      if (rlen == 0)
         continue;
      if (strncmp(cwd, state->worktrees[i].git_root, rlen) == 0 &&
          (cwd[rlen] == '/' || cwd[rlen] == '\0'))
      {
         if (rlen > best_len)
         {
            best = i;
            best_len = rlen;
         }
      }
   }

   if (best >= 0)
   {
      /* Check if already in the worktree; don't redirect. */
      size_t wt_len = strlen(state->worktrees[best].worktree_path);
      if (strncmp(cwd, state->worktrees[best].worktree_path, wt_len) == 0 &&
          (cwd[wt_len] == '/' || cwd[wt_len] == '\0'))
         return NULL;
      return state->worktrees[best].worktree_path;
   }
   return NULL;
}
