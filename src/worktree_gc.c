#include "worktree_gc.h"

#include "log.h"
#include "util.h"
#include "workspace.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define WORKTREE_GC_DEFAULT_DAYS 14

void worktree_gc_options_init(worktree_gc_options_t *opts)
{
   if (!opts)
      return;
   opts->max_age_days = WORKTREE_GC_DEFAULT_DAYS;
   opts->force = 0;
   opts->dry_run = 0;
}

/* Trim trailing whitespace in-place. */
static void rtrim(char *s)
{
   if (!s)
      return;
   size_t n = strlen(s);
   while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
      s[--n] = '\0';
}

/* Best-effort detection of the base branch (main / master / trunk). Falls
 * back to "main" if the inspection commands fail. */
static void detect_base_branch(const char *git_root, char *out, size_t cap)
{
   if (!out || cap == 0)
      return;
   snprintf(out, cap, "main");
   if (!git_root || !git_root[0])
      return;

   char cmd[MAX_PATH_LEN + 128];
   /* Prefer the symbolic-ref of origin/HEAD when set; falls back to
    * checking which of {main,master,trunk} exists locally. */
   snprintf(cmd, sizeof(cmd),
            "git -C '%s' symbolic-ref --short refs/remotes/origin/HEAD 2>/dev/null", git_root);
   int rc;
   char *outbuf = run_cmd(cmd, &rc);
   if (rc == 0 && outbuf && outbuf[0])
   {
      rtrim(outbuf);
      const char *slash = strrchr(outbuf, '/');
      const char *name = slash ? slash + 1 : outbuf;
      if (name && name[0])
         snprintf(out, cap, "%s", name);
      free(outbuf);
      return;
   }
   free(outbuf);

   const char *fallbacks[] = {"main", "master", "trunk", NULL};
   for (int i = 0; fallbacks[i]; i++)
   {
      snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse --verify --quiet '%s' >/dev/null 2>&1",
               git_root, fallbacks[i]);
      char *o = run_cmd(cmd, &rc);
      free(o);
      if (rc == 0)
      {
         snprintf(out, cap, "%s", fallbacks[i]);
         return;
      }
   }
}

static int git_branch_of_worktree(const char *wt_path, char *branch, size_t cap)
{
   if (!wt_path || !branch || cap == 0)
      return -1;
   char cmd[MAX_PATH_LEN + 128];
   snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse --abbrev-ref HEAD 2>/dev/null", wt_path);
   int rc;
   char *o = run_cmd(cmd, &rc);
   if (rc != 0 || !o)
   {
      free(o);
      branch[0] = '\0';
      return -1;
   }
   rtrim(o);
   snprintf(branch, cap, "%s", o);
   free(o);
   return 0;
}

static time_t git_head_time(const char *wt_path)
{
   if (!wt_path)
      return 0;
   char cmd[MAX_PATH_LEN + 128];
   snprintf(cmd, sizeof(cmd), "git -C '%s' log -1 --format=%%ct HEAD 2>/dev/null", wt_path);
   int rc;
   char *o = run_cmd(cmd, &rc);
   time_t t = 0;
   if (rc == 0 && o)
   {
      rtrim(o);
      if (o[0])
         t = (time_t)strtoll(o, NULL, 10);
   }
   free(o);
   return t;
}

static int git_uncommitted(const char *wt_path)
{
   char cmd[MAX_PATH_LEN + 128];
   snprintf(cmd, sizeof(cmd), "git -C '%s' status --porcelain 2>/dev/null", wt_path);
   int rc;
   char *o = run_cmd(cmd, &rc);
   int dirty = (o && o[0] != '\0');
   free(o);
   return dirty;
}

static int git_commits_ahead(const char *wt_path, const char *base_branch)
{
   if (!wt_path || !base_branch || !base_branch[0])
      return -1;
   char cmd[MAX_PATH_LEN + 256];
   snprintf(cmd, sizeof(cmd), "git -C '%s' rev-list --count '%s..HEAD' 2>/dev/null", wt_path,
            base_branch);
   int rc;
   char *o = run_cmd(cmd, &rc);
   int n = -1;
   if (rc == 0 && o)
   {
      rtrim(o);
      if (o[0])
         n = atoi(o);
   }
   free(o);
   return n;
}

static time_t dir_mtime(const char *path)
{
   struct stat st;
   if (stat(path, &st) != 0)
      return 0;
   return st.st_mtime;
}

static void evaluate_candidate(worktree_gc_candidate_t *c, const worktree_gc_options_t *opts,
                               time_t now, int age_secs)
{
   if (!c || !opts)
      return;

   if (c->has_uncommitted)
   {
      c->eligible = 0;
      snprintf(c->reason, sizeof(c->reason), "skip: uncommitted changes");
      return;
   }

   long idle = (long)(now - c->last_activity);
   if (idle < age_secs)
   {
      c->eligible = 0;
      snprintf(c->reason, sizeof(c->reason), "skip: idle %ldd < %dd threshold", idle / 86400,
               opts->max_age_days);
      return;
   }

   if (c->commits_ahead > 0 && !opts->force)
   {
      c->eligible = 0;
      snprintf(c->reason, sizeof(c->reason),
               "skip: %d commit(s) ahead of base branch (use --force to override)",
               c->commits_ahead);
      return;
   }

   c->eligible = 1;
   if (c->commits_ahead > 0)
      snprintf(c->reason, sizeof(c->reason),
               "remove: %d commit(s) ahead but --force; idle %ldd >= %dd", c->commits_ahead,
               idle / 86400, opts->max_age_days);
   else
      snprintf(c->reason, sizeof(c->reason), "remove: 0 commits ahead, idle %ldd >= %dd",
               idle / 86400, opts->max_age_days);
}

int worktree_gc_scan(const char *git_root, const worktree_gc_options_t *opts,
                     worktree_gc_candidate_t *out, int max_out)
{
   if (!git_root || !opts || !out || max_out <= 0)
      return -1;

   char roots_dir[MAX_PATH_LEN];
   snprintf(roots_dir, sizeof(roots_dir), "%s/.aimee/worktrees", git_root);

   DIR *managed = opendir(roots_dir);
   if (!managed)
      return 0; /* no worktrees yet — clean by definition */

   char base_branch[WORKTREE_GC_BRANCH_LEN];
   detect_base_branch(git_root, base_branch, sizeof(base_branch));

   time_t now = time(NULL);
   int age_secs = opts->max_age_days * 86400;
   int count = 0;

   struct dirent *sid_ent;
   while ((sid_ent = readdir(managed)) != NULL && count < max_out)
   {
      if (sid_ent->d_name[0] == '.')
         continue;
      char sid_dir[MAX_PATH_LEN];
      snprintf(sid_dir, sizeof(sid_dir), "%s/%s", roots_dir, sid_ent->d_name);
      DIR *inner = opendir(sid_dir);
      if (!inner)
         continue;

      struct dirent *work_ent;
      while ((work_ent = readdir(inner)) != NULL && count < max_out)
      {
         if (work_ent->d_name[0] == '.')
            continue;
         char wt_path[MAX_PATH_LEN];
         snprintf(wt_path, sizeof(wt_path), "%s/%s", sid_dir, work_ent->d_name);
         struct stat st;
         if (stat(wt_path, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;

         worktree_gc_candidate_t *c = &out[count++];
         memset(c, 0, sizeof(*c));
         snprintf(c->path, sizeof(c->path), "%s", wt_path);
         git_branch_of_worktree(wt_path, c->branch, sizeof(c->branch));
         c->has_uncommitted = git_uncommitted(wt_path);
         c->commits_ahead = git_commits_ahead(wt_path, base_branch);

         time_t head_time = git_head_time(wt_path);
         time_t mt = dir_mtime(wt_path);
         c->last_activity = head_time > mt ? head_time : mt;
         if (c->last_activity == 0)
            c->last_activity = now; /* treat unknown as recent */

         evaluate_candidate(c, opts, now, age_secs);
      }
      closedir(inner);
   }
   closedir(managed);
   return count;
}

/* After a merged worktree is removed, delete its branch too — leaving merged
 * branches behind is what lets `git branch` accumulate hundreds of dead refs.
 * Only called for candidates with 0 commits ahead of base, so `-D` discards
 * nothing. Refuses the base branch, obvious protected names, a detached-HEAD
 * sentinel, and any name unsafe to interpolate into a shell command. */
static void worktree_gc_delete_branch(const char *git_root, const char *branch,
                                      const char *base_branch)
{
   if (!branch || !branch[0])
      return;
   if (strcmp(branch, "HEAD") == 0 || strcmp(branch, "main") == 0 ||
       strcmp(branch, "master") == 0 || strcmp(branch, "trunk") == 0)
      return;
   if (base_branch && base_branch[0] && strcmp(branch, base_branch) == 0)
      return;
   if (branch[0] == '-' || strchr(branch, '\'') != NULL)
      return;

   char cmd[MAX_PATH_LEN + 256];
   snprintf(cmd, sizeof(cmd), "git -C '%s' branch -D '%s' 2>&1", git_root, branch);
   int rc;
   char *o = run_cmd(cmd, &rc);
   if (rc == 0)
      LOG_INFO("worktree_gc", "deleted merged branch '%s'", branch);
   free(o);
}

int worktree_gc_apply(const char *git_root, const worktree_gc_candidate_t *cands, int n,
                      const worktree_gc_options_t *opts)
{
   if (!git_root || !cands || !opts || n <= 0)
      return 0;

   char base_branch[WORKTREE_GC_BRANCH_LEN];
   detect_base_branch(git_root, base_branch, sizeof(base_branch));

   int removed = 0;
   for (int i = 0; i < n; i++)
   {
      const worktree_gc_candidate_t *c = &cands[i];
      if (!c->eligible)
         continue;
      if (opts->dry_run)
      {
         removed++;
         continue;
      }

      /* Parse the candidate path back into (sid, work_name) so we can
       * call the existing cleanup primitive. Path shape:
       *   <git_root>/.aimee/worktrees/<sid>/<work_name>
       * `worktree_cleanup` itself refuses to remove a worktree with
       * uncommitted changes or unpushed commits, so passing through it
       * is safe even if our scan-time check raced the operator. */
      const char *base = c->path + strlen(git_root);
      if (*base == '/')
         base++;
      const char *prefix = ".aimee/worktrees/";
      if (strncmp(base, prefix, strlen(prefix)) != 0)
         continue;
      const char *sid_start = base + strlen(prefix);
      const char *sep = strchr(sid_start, '/');
      if (!sep)
         continue;
      char sid[64];
      size_t sid_len = (size_t)(sep - sid_start);
      if (sid_len == 0 || sid_len >= sizeof(sid))
         continue;
      memcpy(sid, sid_start, sid_len);
      sid[sid_len] = '\0';
      const char *work_name = sep + 1;
      if (!work_name[0])
         continue;

      worktree_cleanup(git_root, sid, work_name);
      /* worktree_cleanup logs its own outcome; assume success only if
       * the directory is gone afterwards. */
      struct stat st;
      if (stat(c->path, &st) != 0)
      {
         removed++;
         /* The worktree is gone and merged (0 commits ahead of base, so
          * nothing is lost) — delete its now-orphaned branch as well. A
          * candidate removed only because of --force (commits ahead) keeps
          * its branch. */
         if (c->commits_ahead == 0)
            worktree_gc_delete_branch(git_root, c->branch, base_branch);
      }
   }
   return removed;
}
