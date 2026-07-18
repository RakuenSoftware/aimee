/* guardrails_tdd.c: TDD-mode helpers and the `post_tool_update` bookkeeping
 * that runs after every tool call. Split from guardrails.c so the two
 * enforcement paths (pre-tool checks and TDD state) can evolve
 * independently. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aimee.h"
#include "cJSON.h"
#include "guardrails_internal.h"
#include "git_verify.h"
#include "log.h"
#include "platform_path.h"
#include "slop_detect.h"
#include <ctype.h>
#include <dirent.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- TDD enforcement --- */

int is_test_file(const char *path)
{
   if (!path)
      return 0;

   const char *dirs[] = {"/test/", "/tests/", "/spec/", "/__tests__/"};
   for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
   {
      if (strstr(path, dirs[i]))
         return 1;
   }

   const char *base = strrchr(path, '/');
   base = base ? base + 1 : path;

   char stem[256];
   snprintf(stem, sizeof(stem), "%s", base);
   char *dot = strrchr(stem, '.');
   if (dot)
      *dot = '\0';

   if (strncmp(stem, "test_", 5) == 0)
      return 1;

   size_t slen = strlen(stem);
   if (slen > 5 && strcmp(stem + slen - 5, "_test") == 0)
      return 1;

   if (strstr(base, ".test."))
      return 1;

   if (slen > 5 && strcmp(stem + slen - 5, "_spec") == 0)
      return 1;
   if (strstr(base, ".spec."))
      return 1;

   if (slen > 5 && strcmp(stem + slen - 5, "Tests") == 0)
      return 1;
   if (slen > 4 && strcmp(stem + slen - 4, "Test") == 0)
      return 1;
   if (slen > 4 && strcmp(stem + slen - 4, "Spec") == 0)
      return 1;

   return 0;
}

int is_tdd_source_file(const char *path)
{
   if (!path)
      return 0;
   const char *ext = strrchr(path, '.');
   if (!ext)
      return 0;
   const char *exts[] = {".c",   ".h",   ".cpp", ".cc",   ".cxx", ".go", ".py", ".ts", ".js",
                         ".jsx", ".tsx", ".rb",  ".java", ".rs",  ".kt", ".cs", NULL};
   for (int i = 0; exts[i]; i++)
   {
      if (strcmp(ext, exts[i]) == 0)
         return 1;
   }
   return 0;
}

static void tdd_extract_stem(const char *path, char *out, size_t out_len)
{
   const char *base = strrchr(path, '/');
   base = base ? base + 1 : path;

   char s[256];
   snprintf(s, sizeof(s), "%s", base);
   char *dot = strrchr(s, '.');
   if (dot)
      *dot = '\0';

   if (strncmp(s, "test_", 5) == 0)
      memmove(s, s + 5, strlen(s + 5) + 1);

   size_t slen = strlen(s);
   if (slen > 5 && strcmp(s + slen - 5, "_test") == 0)
      s[slen - 5] = '\0';
   slen = strlen(s);
   if (slen > 5 && strcmp(s + slen - 5, "_spec") == 0)
      s[slen - 5] = '\0';
   slen = strlen(s);
   if (slen > 5 && strcmp(s + slen - 5, "Tests") == 0)
      s[slen - 5] = '\0';
   else if (slen > 4 && strcmp(s + slen - 4, "Test") == 0)
      s[slen - 4] = '\0';
   else if (slen > 4 && strcmp(s + slen - 4, "Spec") == 0)
      s[slen - 4] = '\0';

   snprintf(out, out_len, "%s", s);
}

static int tdd_has_matching_test(const session_state_t *state, const char *stem)
{
   for (int i = 0; i < state->tdd_write_count; i++)
   {
      if (state->tdd_writes[i].is_test && strcmp(state->tdd_writes[i].stem, stem) == 0)
         return 1;
   }
   return 0;
}

static void tdd_record_write(session_state_t *state, const char *path)
{
   if (!state || !path)
      return;
   if (state->tdd_mode[0] == '\0' || strcmp(state->tdd_mode, "off") == 0)
      return;
   if (!is_tdd_source_file(path))
      return;
   if (state->tdd_write_count >= MAX_TDD_WRITES)
      return;

   char stem[MAX_TDD_STEM];
   tdd_extract_stem(path, stem, sizeof(stem));

   tdd_write_t *w = &state->tdd_writes[state->tdd_write_count++];
   snprintf(w->stem, sizeof(w->stem), "%s", stem);
   w->is_test = is_test_file(path);
}

int tdd_check_impl_write(const session_state_t *state, const char *path, char *msg_buf,
                         size_t msg_len)
{
   if (!state || !path)
      return 0;
   if (state->tdd_mode[0] == '\0' || strcmp(state->tdd_mode, "off") == 0)
      return 0;
   if (!is_tdd_source_file(path))
      return 0;
   if (is_test_file(path))
      return 0;

   char stem[MAX_TDD_STEM];
   tdd_extract_stem(path, stem, sizeof(stem));

   if (tdd_has_matching_test(state, stem))
      return 0;

   const char *base = strrchr(path, '/');
   base = base ? base + 1 : path;

   LOG_WARN("guardrails", "tdd: %s written without test (stem: \"%s\")", base, stem);

   if (strcmp(state->tdd_mode, "enforce") == 0)
   {
      snprintf(msg_buf, msg_len,
               "BLOCKED: TDD enforce mode — write a test file for \"%s\" before implementing %s. "
               "See: aimee skill show test-driven-development.",
               stem, base);
      return 2;
   }

   snprintf(msg_buf, msg_len,
            "ADVISORY: TDD — %s written without a prior test. "
            "Consider writing the test first to define expected behavior. "
            "See: aimee skill show test-driven-development.",
            base);
   return 0;
}

void post_tool_update(const char *tool_name, const char *input_json, session_state_t *state)
{
   if (!tool_name || !input_json)
      return;

   /* Bash: inspect the command for learnable workflow signals. */
   if (is_shell_tool(tool_name))
   {
      cJSON *broot = cJSON_Parse(input_json);
      if (broot)
      {
         cJSON *bcmd = cJSON_GetObjectItem(broot, "command");
         if (bcmd && cJSON_IsString(bcmd) && bcmd->valuestring)
            workflow_observe_bash(bcmd->valuestring);
         cJSON_Delete(broot);
      }
      /* Fall through — no read/edit-specific work for shell tools. */
      return;
   }

   int is_read = strcmp(tool_name, "Read") == 0 || strcmp(tool_name, "read_file") == 0;

   if (!is_edit_tool(tool_name) && !is_read)
      return;

   cJSON *root = cJSON_Parse(input_json);
   if (!root)
      return;

   /* Extract the tool's explicit working directory (workdir/cwd field) so that
    * relative paths in Read and Edit/Write are normalized consistently.  This
    * matches the logic in guardrails_orchestrator.c's tool_effective_cwd(). */
   static const char *cwd_keys[] = {"workdir", "cwd", "working_dir", "working_directory", NULL};
   const char *tool_cwd = NULL;
   for (int ki = 0; cwd_keys[ki]; ki++)
   {
      cJSON *j = cJSON_GetObjectItemCaseSensitive(root, cwd_keys[ki]);
      if (cJSON_IsString(j) && j->valuestring && j->valuestring[0])
      {
         tool_cwd = j->valuestring;
         break;
      }
   }

   /* Read tool: record the file in the session read-set and cache its hash. */
   if (is_read && state)
   {
      cJSON *fp_r = cJSON_GetObjectItem(root, "file_path");
      if (fp_r && cJSON_IsString(fp_r) && fp_r->valuestring && fp_r->valuestring[0])
      {
         char norm[MAX_PATH_LEN];
         normalize_path(fp_r->valuestring, tool_cwd, norm, sizeof(norm));
         session_record_read(state, norm);
      }
      cJSON_Delete(root);
      return;
   }

   cJSON *fp = cJSON_GetObjectItem(root, "file_path");
   if (fp && cJSON_IsString(fp))
   {
      /* Track TDD write ordering */
      tdd_record_write(state, fp->valuestring);

      /* Normalize path before comparing against project roots */
      char norm[MAX_PATH_LEN];
      normalize_path(fp->valuestring, tool_cwd, norm, sizeof(norm));

      /* Refresh the cached content hash for this file. After an edit, the
       * agent's own write is the current truth; without this, a second
       * Edit in the same session trips the stale-edit guard against the
       * agent's prior change rather than an external modification. */
      if (state)
         session_record_read(state, norm);

      /* Per-file canonical re-index has been removed; the canonical
       * index is owned by aimee-kb on a 60-second cooldown. Branch-
       * local edits will be reflected in the next kb scan, and Phase 2
       * adds a DB1 branch overlay so reads pick up the worktree's
       * unscanned changes immediately. */

      /* Advisory slop scan: warn when the written file contains common
       * AI-slop patterns.  Never blocks; output goes to stderr only. */
      slop_finding_t slop[16];
      int nslop = slop_detect_file(norm, slop, 16);
      if (nslop > 0)
      {
         LOG_INFO("guardrails", "slop advisory (%d finding(s) in %s)", nslop, fp->valuestring);
         for (int s = 0; s < nslop; s++)
            LOG_INFO("guardrails", "  %s:%d [%s] %s", fp->valuestring, slop[s].line_number,
                     slop_category_label(slop[s].category), slop[s].excerpt);
         LOG_INFO("guardrails", "  (run 'aimee slop %s' for full report)", fp->valuestring);
      }
   }

   cJSON_Delete(root);
}

/* Resolve the git repository root for a directory. If we're inside a git
 * worktree (e.g. Claude Code's .claude/worktrees/), resolve back to the
 * main repository root via --git-common-dir. */
int git_repo_root(const char *dir, char *out_root, size_t out_len)
{
   char cmd[MAX_PATH_LEN + 128];
   int rc;

   /* First try: detect if we're in a worktree by comparing --git-dir and
    * --git-common-dir. If they differ, --git-common-dir points to the main
    * repo's .git directory, and its parent is the true project root. */
   snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse --git-dir --git-common-dir 2>/dev/null", dir);
   char *out = run_cmd(cmd, &rc);
   if (rc == 0 && out && out[0])
   {
      /* Output is two lines: git-dir\ngit-common-dir\n */
      char *nl = strchr(out, '\n');
      if (nl)
      {
         *nl = '\0';
         char *git_dir = out;
         char *common_dir = nl + 1;
         /* Trim trailing newline from common_dir */
         size_t clen = strlen(common_dir);
         while (clen > 0 && (common_dir[clen - 1] == '\n' || common_dir[clen - 1] == '\r'))
            common_dir[--clen] = '\0';

         if (strcmp(git_dir, common_dir) != 0 && clen > 0)
         {
            /* We're in a worktree. common_dir is the main repo's .git dir.
             * Resolve it to an absolute path, then take its parent. */
            char abs_common[MAX_PATH_LEN];
            if (common_dir[0] == '/')
            {
               snprintf(abs_common, sizeof(abs_common), "%s", common_dir);
            }
            else
            {
               /* Relative path — resolve relative to git-dir */
               char abs_git_dir[MAX_PATH_LEN];
               if (git_dir[0] == '/')
                  snprintf(abs_git_dir, sizeof(abs_git_dir), "%s", git_dir);
               else
                  snprintf(abs_git_dir, sizeof(abs_git_dir), "%s/%s", dir, git_dir);

               snprintf(abs_common, sizeof(abs_common), "%s/%s", abs_git_dir, common_dir);
            }

            /* Canonicalize to resolve any ../ components */
            char *resolved = realpath(abs_common, NULL);
            if (resolved)
            {
               /* Strip trailing /.git to get repo root */
               size_t rlen = strlen(resolved);
               if (rlen >= 5 && strcmp(resolved + rlen - 5, "/.git") == 0)
                  resolved[rlen - 5] = '\0';
               snprintf(out_root, out_len, "%s", resolved);
               free(resolved);
               free(out);
               return 0;
            }
            free(resolved);
         }
      }
   }
   free(out);

   /* Fallback: simple --show-toplevel (works for non-worktree repos) */
   snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse --show-toplevel 2>/dev/null", dir);
   out = run_cmd(cmd, &rc);
   if (rc != 0 || !out || !out[0])
   {
      free(out);
      return -1;
   }
   size_t len = strlen(out);
   while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
      out[--len] = '\0';
   snprintf(out_root, out_len, "%s", out);
   free(out);
   return 0;
}

/* (worktree_entry_init removed — replaced by simple sibling worktree model) */
