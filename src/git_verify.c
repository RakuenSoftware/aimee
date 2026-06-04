/* git_verify.c -- project verification runner.
 *
 * Runs configured verification steps with dependency ordering, caches clean-tree
 * step results, and records .aimee/.last-verify for the merge/push gate.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <ctype.h>

#include "headers/aimee_home.h"
#include "headers/compute_pool.h"
#include "headers/git_verify.h"
#include "headers/git_verify_internal.h"
#include "headers/git_verify_select.h"
#include "headers/git_verify_jobs.h"
#include "headers/aimee.h"
#include "headers/config.h"
#include "headers/util.h"
#include "headers/dstr.h"
#include "headers/log.h"
#include "headers/mcp_git.h"
#include "headers/platform_path.h"

/* --- Helpers --- */

static cJSON *mcp_text(const char *text)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON *item = cJSON_CreateObject();
   cJSON_AddStringToObject(item, "type", "text");
   cJSON_AddStringToObject(item, "text", text);
   cJSON_AddItemToArray(arr, item);
   return arr;
}

/* --- Config loading ---
 * Verify config lives at ~/.config/aimee/projects/<name>/project.yaml and is
 * shared by worktrees via the main repo basename. It is generated once from
 * Makefile targets and then left user-editable. */

/* Resolve to the canonical main-repo root; worktrees share the same name. */
int resolve_main_repo_root(const char *dir, char *out, size_t out_len)
{
   char cmd[MAX_PATH_LEN + 96];
   int rc;

   if (dir && dir[0])
      snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse --git-common-dir 2>/dev/null", dir);
   else
      snprintf(cmd, sizeof(cmd), "git rev-parse --git-common-dir 2>/dev/null");

   char *common = run_cmd(cmd, &rc);
   if (rc == 0 && common && common[0])
   {
      char *nl = strchr(common, '\n');
      if (nl)
         *nl = '\0';
      if (common[0] == '/')
      {
         char *git_suffix = strstr(common, "/.git");
         if (git_suffix)
         {
            *git_suffix = '\0';
            snprintf(out, out_len, "%s", common);
            free(common);
            return 0;
         }
      }
   }
   free(common);

   if (dir && dir[0])
      snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse --show-toplevel 2>/dev/null", dir);
   else
      snprintf(cmd, sizeof(cmd), "git rev-parse --show-toplevel 2>/dev/null");
   char *top = run_cmd(cmd, &rc);
   if (rc == 0 && top && top[0])
   {
      char *nl = strchr(top, '\n');
      if (nl)
         *nl = '\0';
      snprintf(out, out_len, "%s", top);
      free(top);
      return 0;
   }
   free(top);

   if (dir && dir[0])
   {
      snprintf(out, out_len, "%s", dir);
      return 0;
   }
   if (getcwd(out, out_len))
      return 0;
   return -1;
}

/* Resolve to the current checkout's top-level directory. In a worktree this is
 * the worktree root, not the shared main repo, so per-worktree verify state is
 * recorded alongside the checkout that ran the verification. */
static int resolve_verify_root(const char *dir, char *out, size_t out_len)
{
   char cmd[MAX_PATH_LEN + 64];
   int rc;

   if (dir && dir[0])
      snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse --show-toplevel 2>/dev/null", dir);
   else
      snprintf(cmd, sizeof(cmd), "git rev-parse --show-toplevel 2>/dev/null");

   char *top = run_cmd(cmd, &rc);
   if (rc == 0 && top && top[0])
   {
      char *nl = strchr(top, '\n');
      if (nl)
         *nl = '\0';
      snprintf(out, out_len, "%s", top);
      free(top);
      return 0;
   }
   free(top);

   if (dir && dir[0])
   {
      snprintf(out, out_len, "%s", dir);
      return 0;
   }
   if (getcwd(out, out_len))
      return 0;
   return -1;
}

/* Derive the project directory name from a project root path (or CWD).
 * Resolves to the canonical main-repo root so that every worktree of a repo
 * shares one verify config keyed by the real repo name. A worktree's own
 * toplevel basename is unsuitable here: session worktrees live at
 * .aimee/worktrees/<hash>/main, so the basename is the literal "main" for
 * every repo, which collides all worktrees onto a single projects/main config.
 * Writes into buf and returns buf, or NULL on failure. */
static const char *project_dirname(const char *project_root, char *buf, size_t len)
{
   char resolved[MAX_PATH_LEN];
   if (resolve_main_repo_root(project_root, resolved, sizeof(resolved)) != 0)
      return NULL;

   size_t rlen = strlen(resolved);
   while (rlen > 1 && resolved[rlen - 1] == '/')
      resolved[--rlen] = '\0';

   const char *base = strrchr(resolved, '/');
   base = base ? base + 1 : resolved;
   snprintf(buf, len, "%s", base);
   return buf;
}

/* Locate the directory containing the project's Makefile. Checks the
 * project root first, then a `src/` subdir. Writes a path relative to
 * the project root into subdir_out (empty string for root). Returns 0
 * on success, -1 if no Makefile is found. */
static int find_makefile_subdir(const char *project_root, char *subdir_out, size_t subdir_len)
{
   char path[MAX_PATH_LEN];
   struct stat st;

   snprintf(path, sizeof(path), "%s/Makefile", project_root);
   if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
   {
      snprintf(subdir_out, subdir_len, "%s", "");
      return 0;
   }

   snprintf(path, sizeof(path), "%s/src/Makefile", project_root);
   if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
   {
      snprintf(subdir_out, subdir_len, "%s", "src");
      return 0;
   }

   return -1;
}

static int make_output_has_target(const char *make_output, const char *target)
{
   if (!make_output || !target || !target[0])
      return 0;

   size_t tlen = strlen(target);
   if (strncmp(make_output, target, tlen) == 0 && make_output[tlen] == ':')
      return 1;

   char needle[128];
   snprintf(needle, sizeof(needle), "\n%s:", target);
   return strstr(make_output, needle) != NULL;
}

static int append_verify_local_step(dstr_t *yaml, const char *make_subdir)
{
   dstr_appendf(yaml, "    - name: verify-local\n");
   if (make_subdir && make_subdir[0])
      dstr_appendf(yaml,
                   "      run: cd %s && make -j${AIMEE_VERIFY_MAKE_JOBS:-$(nproc 2>/dev/null || "
                   "echo 4)} AIMEE_VERIFY_TEST_JOBS=${AIMEE_VERIFY_TEST_JOBS:-$(nproc 2>/dev/null "
                   "|| echo 4)} verify-local\n",
                   make_subdir);
   else
      dstr_appendf(
          yaml, "      run: make -j${AIMEE_VERIFY_MAKE_JOBS:-$(nproc 2>/dev/null || echo 4)} "
                "AIMEE_VERIFY_TEST_JOBS=${AIMEE_VERIFY_TEST_JOBS:-$(nproc 2>/dev/null || echo 4)} "
                "verify-local\n");
   return 1;
}

/* Generate ~/.config/aimee/projects/<name>/project.yaml for the given
 * project by introspecting its Makefile and emitting a verify step for
 * the repo's verify-local target, or each well-known target found when no
 * verify-local target exists. Returns 0 on success, -1 on failure
 * (no Makefile, no recognized targets, or write error).
 *
 * The generated file is never overwritten on subsequent calls — callers
 * must check existence first. */
static int generate_project_yaml(const char *project_root, const char *output_path)
{
   char root[MAX_PATH_LEN];
   if (resolve_verify_root(project_root, root, sizeof(root)) != 0)
      return -1;

   char make_subdir[MAX_PATH_LEN];
   if (find_makefile_subdir(root, make_subdir, sizeof(make_subdir)) != 0)
      return -1;

   /* Run `make -pn` to enumerate targets without executing recipes. */
   char cmd[2 * MAX_PATH_LEN + 64];
   if (make_subdir[0])
      snprintf(cmd, sizeof(cmd), "cd '%s/%s' && make -pn 2>/dev/null", root, make_subdir);
   else
      snprintf(cmd, sizeof(cmd), "cd '%s' && make -pn 2>/dev/null", root);

   int rc;
   char *out = run_cmd(cmd, &rc);
   if (!out)
      return -1;

   /* Prefer a repo-defined `verify-local` target when it exists. It is the
    * project's curated fast local gate and can encode repo-specific choices
    * such as serial test execution or omitting heavier CI-only checks.
    *
    * Without verify-local, fall back to well-known targets. `all` becomes a
    * "build" step so unit-tests can declare `after: build`; `test` is
    * suppressed when `unit-tests` is also present. Prefer `check-linking`
    * over `all` when it exists because it links every shippable binary. */
   static const char *KNOWN[] = {"lint", "all", "unit-tests", "test", "build-integrity", NULL};

   /* Auto-generated config gates pushes/PRs only when the global verify master
    * switch is on; with verify disabled (default) it is generated non-enforcing
    * so explicit `aimee git verify` runs still work without imposing a gate.
    * Users opt a project in by editing this to enforce: true. */
   const char *enforce_default = verify_enabled_global() ? "true" : "false";

   dstr_t yaml;
   dstr_init(&yaml);
   dstr_append_str(&yaml, "# Auto-generated by aimee on first verify. Edit freely —\n"
                          "# aimee will not regenerate this file unless it is removed.\n"
                          "verify:\n");
   dstr_appendf(&yaml, "  enforce: %s\n", enforce_default);
   dstr_append_str(&yaml, "  steps:\n");

   int emitted = 0;
   int has_unit_tests = 0;
   if (make_output_has_target(out, "verify-local"))
   {
      emitted += append_verify_local_step(&yaml, make_subdir);
   }
   else
   {
      for (int i = 0; KNOWN[i]; i++)
      {
         const char *target = KNOWN[i];
         if (strcmp(target, "all") == 0 && make_output_has_target(out, "check-linking"))
            target = "check-linking";

         if (strcmp(target, "test") == 0 && has_unit_tests)
            continue;

         if (!make_output_has_target(out, target))
            continue;

         const char *step_name =
             (strcmp(target, "all") == 0 || strcmp(target, "check-linking") == 0) ? "build"
                                                                                  : target;
         dstr_appendf(&yaml, "    - name: %s\n", step_name);
         const int is_test_target =
             (strcmp(target, "unit-tests") == 0 || strcmp(target, "test") == 0);
         const char *test_jobs =
             is_test_target
                 ? " TEST_RUN_JOBS=${AIMEE_VERIFY_TEST_JOBS:-$(nproc 2>/dev/null || echo 4)}"
                 : "";
         if (make_subdir[0])
            dstr_appendf(&yaml,
                         "      run: cd %s && make "
                         "-j${AIMEE_VERIFY_MAKE_JOBS:-$(nproc 2>/dev/null || echo 4)}%s %s\n",
                         make_subdir, test_jobs, target);
         else
            dstr_appendf(&yaml,
                         "      run: make -j${AIMEE_VERIFY_MAKE_JOBS:-$(nproc 2>/dev/null || "
                         "echo 4)}%s %s\n",
                         test_jobs, target);

         /* unit-tests depends on the build step completing first — both hit
          * the same Makefile and race on object files if run in parallel.
          * build-integrity also runs isolated make builds internally, so keep it
          * behind unit-tests when they exist; otherwise it can race with the
          * verify unit-test wave on generated build artifacts. */
         if (strcmp(target, "unit-tests") == 0 || strcmp(target, "test") == 0)
            dstr_appendf(&yaml, "      after: build\n");
         else if (strcmp(target, "build-integrity") == 0)
            dstr_appendf(&yaml, "      after: %s\n", has_unit_tests ? "unit-tests" : "build");

         if (strcmp(target, "unit-tests") == 0 || strcmp(target, "test") == 0)
            has_unit_tests = 1;
         emitted++;
      }
   }
   free(out);

   if (emitted == 0)
   {
      dstr_free(&yaml);
      return -1;
   }

   /* Create parent directory tree. */
   char dir[MAX_PATH_LEN];
   snprintf(dir, sizeof(dir), "%s", output_path);
   char *slash = strrchr(dir, '/');
   if (slash)
   {
      *slash = '\0';
      platform_mkdir_p(dir, 0755);
   }

   FILE *f = fopen(output_path, "w");
   if (!f)
   {
      dstr_free(&yaml);
      return -1;
   }
   fputs(dstr_cstr(&yaml), f);
   fclose(f);
   dstr_free(&yaml);
   return 0;
}

/* Build the absolute path of the global project.yaml for the given
 * project. Returns 0 on success, -1 if HOME or project name cannot be
 * resolved. Worktrees resolve to the same path as the main checkout. */
int project_yaml_path(const char *project_root, char *out, size_t out_len)
{
   const char *base = aimee_home();
   if (!base)
      return -1;

   char dirname[256];
   if (!project_dirname(project_root, dirname, sizeof(dirname)))
      return -1;

   snprintf(out, out_len, "%s/projects/%s/project.yaml", base, dirname);
   return 0;
}

/* Read the primary_branch field from the project's project.yaml.
 * Writes the branch name into out (up to out_len bytes).
 * Returns 0 if found and non-empty, -1 otherwise. */
int project_primary_branch(const char *project_root, char *out, size_t out_len)
{
   char path[MAX_PATH_LEN];
   if (project_yaml_path(project_root, path, sizeof(path)) != 0)
      return -1;

   FILE *f = fopen(path, "r");
   if (!f)
      return -1;

   int found = 0;
   char line[512];
   while (fgets(line, sizeof(line), f))
   {
      if (strncmp(line, "primary_branch:", 15) == 0)
      {
         const char *val = line + 15;
         while (*val == ' ' || *val == '\t')
            val++;
         size_t len = strlen(val);
         while (len > 0 && (val[len - 1] == '\n' || val[len - 1] == '\r' || val[len - 1] == ' '))
            len--;
         if (len > 0)
         {
            snprintf(out, out_len, "%.*s", (int)len, val);
            found = 1;
         }
         break;
      }
   }
   fclose(f);
   return found ? 0 : -1;
}

/* Open the project.yaml for the given project. Looks only at
 * ~/.config/aimee/projects/<name>/project.yaml. If the file does not
 * exist, attempts to auto-generate it from the project's Makefile and
 * retries the open. Returns NULL if neither resolution nor generation
 * succeeds. */
static FILE *open_project_yaml(const char *project_root)
{
   char path[MAX_PATH_LEN];
   if (project_yaml_path(project_root, path, sizeof(path)) != 0)
      return NULL;

   FILE *f = fopen(path, "r");
   if (f)
      return f;

   if (generate_project_yaml(project_root, path) != 0)
      return NULL;

   return fopen(path, "r");
}

static void verify_normalize_step_order(verify_config_t *cfg)
{
   const char *test_step = NULL;
   if (!cfg)
      return;
   for (int i = 0; i < cfg->count && !test_step; i++)
      if (strcmp(cfg->steps[i].name, "unit-tests") == 0 || strcmp(cfg->steps[i].name, "test") == 0)
         test_step = cfg->steps[i].name;
   if (!test_step)
      return;
   for (int i = 0; i < cfg->count; i++)
      if (strcmp(cfg->steps[i].name, "build-integrity") == 0 &&
          strcmp(cfg->steps[i].after, "build") == 0)
         snprintf(cfg->steps[i].after, MAX_STEP_NAME, "%s", test_step);
}

int verify_load_config(const char *project_root, verify_config_t *cfg)
{
   memset(cfg, 0, sizeof(*cfg));

   FILE *f = open_project_yaml(project_root);
   if (!f)
      return -1;

   int in_verify = 0;
   int in_steps = 0; /* inside the nested steps: sub-key */
   int in_env = 0;
   int pending_name = 0; /* saw a - name: line, expecting run: next */
   char line[1024];

   while (fgets(line, sizeof(line), f))
   {
      /* Strip trailing newline/cr */
      size_t len = strlen(line);
      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
         line[--len] = '\0';

      /* Blank line or comment */
      if (len == 0 || line[0] == '#')
         continue;

      /* Non-indented line: entering or leaving top-level sections */
      if (line[0] != ' ' && line[0] != '\t')
      {
         if (strncmp(line, "verify:", 7) == 0)
         {
            in_verify = 1;
            in_steps = 0;
            in_env = 0;
            continue;
         }
         else if (strncmp(line, "env_check:", 10) == 0)
         {
            in_env = 1;
            in_verify = 0;
            in_steps = 0;
            continue;
         }
         else
         {
            in_verify = 0;
            in_steps = 0;
            in_env = 0;
            continue;
         }
      }

      char *trimmed = line;
      while (*trimmed == ' ' || *trimmed == '\t')
         trimmed++;

      if (in_verify)
      {
         if (strncmp(trimmed, "enforce:", 8) == 0)
         {
            char *val = trimmed + 8;
            while (*val == ' ')
               val++;
            cfg->enforce = (strncmp(val, "true", 4) == 0) ? 1 : 0;
         }
         else if (strncmp(trimmed, "incremental:", 12) == 0)
         {
            char *val = trimmed + 12;
            while (*val == ' ')
               val++;
            cfg->incremental = (strncmp(val, "true", 4) == 0) ? 1 : 0;
         }
         else if (strncmp(trimmed, "always_run_globs:", 17) == 0)
         {
            char *val = trimmed + 17;
            while (*val == ' ')
               val++;
            snprintf(cfg->always_run_globs, sizeof(cfg->always_run_globs), "%s", val);
         }
         else if (strncmp(trimmed, "steps:", 6) == 0)
         {
            in_steps = 1;
         }
         else if (in_steps)
         {
            if (strncmp(trimmed, "- name:", 7) == 0)
            {
               if (cfg->count >= MAX_VERIFY_STEPS)
                  break;
               char *val = trimmed + 7;
               while (*val == ' ')
                  val++;
               snprintf(cfg->steps[cfg->count].name, MAX_STEP_NAME, "%s", val);
               pending_name = 1;
            }
            else if (strncmp(trimmed, "run:", 4) == 0 && pending_name)
            {
               char *val = trimmed + 4;
               while (*val == ' ')
                  val++;
               snprintf(cfg->steps[cfg->count].run, MAX_STEP_CMD, "%s", val);
               cfg->count++;
               pending_name = 0;
            }
            else if (strncmp(trimmed, "after:", 6) == 0 && cfg->count > 0)
            {
               char *val = trimmed + 6;
               while (*val == ' ')
                  val++;
               snprintf(cfg->steps[cfg->count - 1].after, MAX_STEP_NAME, "%s", val);
            }
            else if (strncmp(trimmed, "paths:", 6) == 0 && cfg->count > 0)
            {
               char *val = trimmed + 6;
               while (*val == ' ')
                  val++;
               snprintf(cfg->steps[cfg->count - 1].paths, sizeof(cfg->steps[0].paths), "%s", val);
            }
            else if (strncmp(trimmed, "scope:", 6) == 0 && cfg->count > 0)
            {
               char *val = trimmed + 6;
               while (*val == ' ')
                  val++;
               cfg->steps[cfg->count - 1].scope_changed = (strncmp(val, "changed", 7) == 0) ? 1 : 0;
            }
         }
      }
      else if (in_env)
      {
         /* Inside env_check section: look for "  - tool" */
         if (strncmp(trimmed, "- ", 2) == 0)
         {
            if (cfg->env_count < MAX_ENV_CHECKS)
            {
               char *val = trimmed + 2;
               while (*val == ' ')
                  val++;
               snprintf(cfg->env_checks[cfg->env_count], MAX_STEP_NAME, "%s", val);
               cfg->env_count++;
            }
         }
      }
   }

   fclose(f);
   verify_normalize_step_order(cfg);
   verify_config_prefer_verify_local(project_root, cfg);
   /* Return 0 (success) if either enforce is set, steps are defined, or env checks exist.
    * This allows a verify: section with only enforce: true and no steps to still be
    * detected as a configured verify section. */
   return (cfg->enforce || cfg->count > 0 || cfg->env_count > 0) ? 0 : -1;
}

/* --- Commit-hash-based change detection --- */

char *verify_compute_file_hash(const char *project_root)
{
   /* Return the tree hash (HEAD^{tree}) rather than the commit hash.
    * Tree hashes are stable across squash-merges and rebases that don't change
    * content, so a verified worktree HEAD matches the squash-merge commit that
    * GitHub creates from the same tree. */
   char cmd[MAX_PATH_LEN + 64];
   if (project_root && project_root[0])
      snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse HEAD^{tree} 2>/dev/null", project_root);
   else
      snprintf(cmd, sizeof(cmd), "git rev-parse HEAD^{tree} 2>/dev/null");

   int rc;
   char *out = run_cmd(cmd, &rc);
   if (rc != 0 || !out || !out[0])
   {
      free(out);
      return NULL;
   }

   /* Strip trailing whitespace */
   for (char *p = out + strlen(out) - 1; p >= out && (*p == '\n' || *p == '\r' || *p == ' '); p--)
      *p = '\0';

   char *result = strdup(out);
   free(out);
   return result;
}

/* Return the HEAD commit hash (for display only — not used as the verify key).
 * Caller must free the returned string. Returns NULL on failure. */
static char *verify_compute_commit_hash(const char *project_root)
{
   char cmd[MAX_PATH_LEN + 64];
   if (project_root && project_root[0])
      snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse HEAD 2>/dev/null", project_root);
   else
      snprintf(cmd, sizeof(cmd), "git rev-parse HEAD 2>/dev/null");

   int rc;
   char *out = run_cmd(cmd, &rc);
   if (rc != 0 || !out || !out[0])
   {
      free(out);
      return NULL;
   }
   for (char *p = out + strlen(out) - 1; p >= out && (*p == '\n' || *p == '\r' || *p == ' '); p--)
      *p = '\0';
   char *result = strdup(out);
   free(out);
   return result;
}

static int verify_worktree_has_changes(const char *project_root)
{
   char cmd[MAX_PATH_LEN + 64];
   int rc;
   if (project_root && project_root[0])
      snprintf(cmd, sizeof(cmd), "git -C '%s' status --porcelain 2>/dev/null", project_root);
   else
      snprintf(cmd, sizeof(cmd), "git status --porcelain 2>/dev/null");
   char *status = run_cmd(cmd, &rc);
   int has_changes = (rc == 0 && status && status[0]);
   free(status);
   return has_changes;
}

/* --- State file management --- */

void verify_state_path(const char *project_root, char *buf, size_t len)
{
   /* Always write to the main checkout, not a worktree-specific path.
    * This lets the pre-push hook (which runs from the main checkout) find
    * verification state that was recorded in any worktree of the same repo. */
   char main_root[MAX_PATH_LEN];
   const char *base = project_root;
   if (project_root && project_root[0] &&
       resolve_main_repo_root(project_root, main_root, sizeof(main_root)) == 0 && main_root[0])
      base = main_root;

   if (base && base[0])
      snprintf(buf, len, "%s/.aimee/.last-verify", base);
   else
      snprintf(buf, len, ".aimee/.last-verify");
}

/* State file format — one entry per line (multi-branch rolling window):
 *   <unix_timestamp> <commit_hash> failed=N/total=M
 *
 * Up to VERIFY_STATE_MAX entries are kept (oldest pruned on write).
 * Legacy single-entry format (timestamp on line 1, hash on line 2, result
 * on line 3) is parsed on read and silently upgraded on next write.
 */
#define VERIFY_STATE_MAX 8

typedef struct
{
   time_t ts;
   char hash[64];
   int failed;
   int total;
   char step_results[256]; /* "name:rc,name:rc,..."; empty if not recorded */
} verify_state_entry_t;

/* Parse the state file into entries[].  Returns the number of entries read
 * (0 if the file doesn't exist or is empty/corrupt). */
static int read_verify_entries(const char *project_root, verify_state_entry_t *entries, int cap)
{
   char path[MAX_PATH_LEN];
   verify_state_path(project_root, path, sizeof(path));

   FILE *f = fopen(path, "r");
   if (!f)
      return 0;

   char line[512];
   int n = 0;

   /* Peek at the first line to detect legacy format (pure integer = old ts line). */
   if (!fgets(line, sizeof(line), f))
   {
      fclose(f);
      return 0;
   }

   /* Strip trailing whitespace */
   char *ep = line + strlen(line) - 1;
   while (ep >= line && (*ep == '\n' || *ep == '\r' || *ep == ' '))
      *ep-- = '\0';

   /* Legacy format: first line is a bare integer (no spaces). */
   if (!strchr(line, ' '))
   {
      if (n < cap)
      {
         time_t ts = (time_t)strtoll(line, NULL, 10);
         char hline[64] = {0};
         char rline[32] = {0};
         if (fgets(hline, sizeof(hline), f))
         {
            char *p = hline + strlen(hline) - 1;
            while (p >= hline && (*p == '\n' || *p == '\r' || *p == ' '))
               *p-- = '\0';
            (void)fgets(rline, sizeof(rline), f);
            p = rline + strlen(rline) - 1;
            while (p >= rline && (*p == '\n' || *p == '\r' || *p == ' '))
               *p-- = '\0';

            if (hline[0])
            {
               entries[n].ts = ts;
               snprintf(entries[n].hash, sizeof(entries[n].hash), "%s", hline);
               entries[n].failed = 0;
               entries[n].total = 0;
               entries[n].step_results[0] = '\0';
               if (rline[0])
                  sscanf(rline, "failed=%d/total=%d", &entries[n].failed, &entries[n].total);
               n++;
            }
         }
      }
      fclose(f);
      return n;
   }

   /* New format: parse the first line we already read, then the rest. */
   for (;;)
   {
      if (line[0] && n < cap)
      {
         long long ts_ll = 0;
         char h[64] = {0};
         int fv = 0, tv = 0;
         if (sscanf(line, "%lld %63s failed=%d/total=%d", &ts_ll, h, &fv, &tv) >= 2 && h[0])
         {
            entries[n].ts = (time_t)ts_ll;
            snprintf(entries[n].hash, sizeof(entries[n].hash), "%s", h);
            entries[n].failed = fv;
            entries[n].total = tv;
            entries[n].step_results[0] = '\0';
            const char *sp = strstr(line, " steps=");
            if (sp)
               snprintf(entries[n].step_results, sizeof(entries[n].step_results), "%s", sp + 7);
            n++;
         }
      }
      if (!fgets(line, sizeof(line), f))
         break;
      ep = line + strlen(line) - 1;
      while (ep >= line && (*ep == '\n' || *ep == '\r' || *ep == ' '))
         *ep-- = '\0';
   }
   fclose(f);
   return n;
}

/* Find the entry in entries[] whose hash matches target_hash (first 40 chars).
 * Returns the index, or -1 if not found. */
static int find_verify_entry(const verify_state_entry_t *entries, int n, const char *target_hash)
{
   for (int i = 0; i < n; i++)
      if (strncmp(entries[i].hash, target_hash, 40) == 0)
         return i;
   return -1;
}

/* Look up a step's recorded exit code in a "name:rc,name:rc,..." string.
 * Returns 1 and sets *out_rc on success, 0 if name not found. */
static int step_result_lookup(const char *step_results, const char *name, int *out_rc)
{
   if (!step_results || !step_results[0] || !name || !out_rc)
      return 0;
   char key[MAX_STEP_NAME + 2];
   snprintf(key, sizeof(key), "%s:", name);
   size_t klen = strlen(key);
   const char *p = step_results;
   while (p && *p)
   {
      if (strncmp(p, key, klen) == 0)
      {
         *out_rc = atoi(p + klen);
         return 1;
      }
      p = strchr(p, ',');
      if (p)
         p++;
   }
   return 0;
}

static int write_verify_state(const char *project_root, time_t timestamp, const char *hash,
                              int failed_steps, int total_steps, const char *step_results)
{
   char path[MAX_PATH_LEN], tmp_path[MAX_PATH_LEN];
   verify_state_path(project_root, path, sizeof(path));
   snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

   char parent[MAX_PATH_LEN];
   snprintf(parent, sizeof(parent), "%s", path);
   char *slash = strrchr(parent, '/');
   if (slash)
   {
      *slash = '\0';
      if (parent[0] && platform_mkdir_p(parent, 0755) != 0 && errno != EEXIST)
         return -1;
   }

   verify_state_entry_t old[VERIFY_STATE_MAX];
   int nold = read_verify_entries(project_root, old, VERIFY_STATE_MAX);

   FILE *f = fopen(tmp_path, "w");
   if (!f)
      return -1;

   if (step_results && step_results[0])
      fprintf(f, "%lld %s failed=%d/total=%d steps=%s\n", (long long)timestamp, hash, failed_steps,
              total_steps, step_results);
   else
      fprintf(f, "%lld %s failed=%d/total=%d\n", (long long)timestamp, hash, failed_steps,
              total_steps);

   int kept = 1;
   for (int i = 0; i < nold && kept < VERIFY_STATE_MAX; i++)
   {
      if (strncmp(old[i].hash, hash, 40) == 0)
         continue;
      if (old[i].step_results[0])
         fprintf(f, "%lld %s failed=%d/total=%d steps=%s\n", (long long)old[i].ts, old[i].hash,
                 old[i].failed, old[i].total, old[i].step_results);
      else
         fprintf(f, "%lld %s failed=%d/total=%d\n", (long long)old[i].ts, old[i].hash,
                 old[i].failed, old[i].total);
      kept++;
   }
   fclose(f);

   if (rename(tmp_path, path) != 0)
   {
      remove(tmp_path);
      return -1;
   }
   return 0;
}

/* --- Parallel step execution --- */

static void format_step_results(const verify_thread_ctx_t *ctxs, int n, char *buf, size_t len)
{
   size_t pos = 0;
   for (int i = 0; i < n && pos + 4 < len; i++)
   {
      if (i > 0)
         buf[pos++] = ',';
      int w = snprintf(buf + pos, len - pos, "%s:%d", ctxs[i].step->name, ctxs[i].rc);
      if (w < 0 || (size_t)w >= len - pos)
         break;
      pos += (size_t)w;
   }
   buf[pos] = '\0';
}

/* Per-step state shared between the dispatcher thread and the worker
 * threads on the compute pool: 0=pending, 1=submitted/running, 2=done. */
typedef struct
{
   verify_thread_ctx_t *ctx;
   int *step_state;
   int *remaining;
   pthread_mutex_t *mutex;
   pthread_cond_t *cond;
} verify_pool_arg_t;

static void verify_pool_worker(void *arg)
{
   verify_pool_arg_t *a = (verify_pool_arg_t *)arg;
   verify_thread_ctx_t *ctx = a->ctx;

   /* Publish this slot's identity so `aimee workers` shows the verify step. */
   compute_pool_set_job(POOL_JOB_VERIFY, "step=%s", ctx->step->name);

   /* Pool worker threads do not inherit the dispatcher's thread-local
    * tl_run_cwd; re-set it here so run_cmd() executes in the correct
    * project directory rather than the server process's CWD. */
   if (ctx->project_root[0])
      run_cmd_set_cwd(ctx->project_root);

   verify_run_step(ctx);

   pthread_mutex_lock(a->mutex);
   *a->step_state = 2;
   (*a->remaining)--;
   pthread_cond_broadcast(a->cond);
   pthread_mutex_unlock(a->mutex);
   free(a);

   compute_pool_clear_job();
}

/* Run inline as a last-resort fallback when no pool is available. */
static void verify_run_inline(verify_config_t *cfg, verify_thread_ctx_t *contexts)
{
   for (int i = 0; i < cfg->count; i++)
   {
      if (contexts[i].project_root[0])
         run_cmd_set_cwd(contexts[i].project_root);
      verify_run_step(&contexts[i]);
   }
}

/* Run verify steps on the supplied pool (NULL = ephemeral, CLI-only fallback).
 * Steps are dispatched as their `after` dependencies clear. */
static int verify_max_parallel_threads(void);
static void verify_run_waves_on_pool(compute_pool_t *external_pool, verify_config_t *cfg,
                                     verify_thread_ctx_t *contexts, const char *pre_hash)
{
   compute_pool_t local_pool;
   compute_pool_t *pool = external_pool;
   int owns_pool = 0;

   if (!pool)
   {
      int max_parallel = verify_max_parallel_threads();
      if (compute_pool_init(&local_pool, max_parallel) != 0)
      {
         verify_run_inline(cfg, contexts);
         return;
      }
      compute_pool_register_secondary(&local_pool, "verify");
      pool = &local_pool;
      owns_pool = 1;
   }

   int step_state[MAX_VERIFY_STEPS];
   memset(step_state, 0, sizeof(step_state));
   int remaining = cfg->count;
   verify_incremental_apply(contexts[0].project_root[0] ? contexts[0].project_root : NULL, cfg,
                            contexts, step_state, &remaining);

   /* Skip steps that already passed at this tree hash (per-step result cache). */
   if (pre_hash && pre_hash[0] && cfg->count > 0)
   {
      const char *root = contexts[0].project_root[0] ? contexts[0].project_root : NULL;
      verify_state_entry_t ents[VERIFY_STATE_MAX];
      int nent = read_verify_entries(root, ents, VERIFY_STATE_MAX);
      int eidx = find_verify_entry(ents, nent, pre_hash);
      if (eidx >= 0 && ents[eidx].step_results[0])
      {
         for (int i = 0; i < cfg->count; i++)
         {
            int saved_rc = -1;
            if (step_state[i] == 0 &&
                step_result_lookup(ents[eidx].step_results, cfg->steps[i].name, &saved_rc) &&
                saved_rc == 0)
            {
               contexts[i].rc = 0;
               contexts[i].skipped = 1;
               step_state[i] = 2;
               remaining--;
            }
         }
      }
   }

   pthread_mutex_t mutex;
   pthread_cond_t cond;
   pthread_mutex_init(&mutex, NULL);
   pthread_cond_init(&cond, NULL);

   pthread_mutex_lock(&mutex);
   while (remaining > 0)
   {
      int submitted_in_pass = 0;
      for (int i = 0; i < cfg->count; i++)
      {
         if (step_state[i] != 0)
            continue;

         if (cfg->steps[i].after[0])
         {
            int dep_done = 0;
            for (int j = 0; j < cfg->count; j++)
            {
               if (strcmp(cfg->steps[j].name, cfg->steps[i].after) == 0 && step_state[j] == 2)
               {
                  dep_done = 1;
                  break;
               }
            }
            if (!dep_done)
               continue;
         }

         verify_pool_arg_t *a = malloc(sizeof(*a));
         if (!a)
         {
            contexts[i].rc = -1;
            step_state[i] = 2;
            remaining--;
            continue;
         }
         a->ctx = &contexts[i];
         a->step_state = &step_state[i];
         a->remaining = &remaining;
         a->mutex = &mutex;
         a->cond = &cond;

         step_state[i] = 1;
         if (compute_pool_submit(pool, verify_pool_worker, a) != 0)
         {
            /* Queue full — leave the step pending and wait for a worker
             * to drain.  The cond_wait below will trip when an existing
             * job finishes, freeing a queue slot. */
            step_state[i] = 0;
            free(a);
            break;
         }
         submitted_in_pass++;
      }

      if (remaining == 0)
         break;

      if (submitted_in_pass == 0)
      {
         int outstanding = 0;
         for (int i = 0; i < cfg->count; i++)
            if (step_state[i] == 1)
               outstanding++;
         if (outstanding == 0)
         {
            /* Nothing pending could be submitted and nothing is running
             * — the remaining steps have unsatisfied deps. */
            for (int i = 0; i < cfg->count; i++)
            {
               if (step_state[i] == 0)
               {
                  fprintf(stderr, "error: step '%s' has unsatisfied dependency '%s'\n",
                          cfg->steps[i].name, cfg->steps[i].after);
                  contexts[i].rc = -1;
                  step_state[i] = 2;
                  remaining--;
               }
            }
            break;
         }
      }

      pthread_cond_wait(&cond, &mutex);
   }
   pthread_mutex_unlock(&mutex);

   if (owns_pool)
   {
      compute_pool_unregister_secondary(&local_pool);
      compute_pool_shutdown(&local_pool);
   }
   pthread_mutex_destroy(&mutex);
   pthread_cond_destroy(&cond);
}

void verify_run_waves(verify_config_t *cfg, verify_thread_ctx_t *contexts)
{
   const char *root =
       cfg->count > 0 && contexts[0].project_root[0] ? contexts[0].project_root : NULL;
   char *pre_hash = verify_compute_file_hash(root);
   verify_run_waves_on_pool(NULL, cfg, contexts,
                            verify_worktree_has_changes(root) ? NULL : pre_hash);
   free(pre_hash);
}

/* --- Check verification state --- */

int verify_check(const char *project_root, const char *expected_commit, char *msg_buf,
                 size_t msg_len)
{
   /* A gate check must never auto-generate config: an unconfigured repo simply
    * has no gate. Only consult an existing project.yaml. */
   {
      char ypath[MAX_PATH_LEN];
      if (project_yaml_path(project_root, ypath, sizeof(ypath)) == 0 && access(ypath, F_OK) != 0)
      {
         if (msg_buf)
            snprintf(msg_buf, msg_len, "no verify steps configured");
         return 1;
      }
   }

   verify_config_t cfg;
   if (verify_load_config(project_root, &cfg) != 0)
   {
      /* No verify section -- no gate */
      if (msg_buf)
         snprintf(msg_buf, msg_len, "no verify steps configured");
      return 1;
   }

   const char *current_hash;
   char *computed_hash = NULL;
   if (expected_commit && expected_commit[0])
   {
      char resolve_cmd[MAX_PATH_LEN + 128];
      if (project_root && project_root[0])
         snprintf(resolve_cmd, sizeof(resolve_cmd), "git -C '%s' rev-parse %s^{tree} 2>/dev/null",
                  project_root, expected_commit);
      else
         snprintf(resolve_cmd, sizeof(resolve_cmd), "git rev-parse %s^{tree} 2>/dev/null",
                  expected_commit);
      int rrc;
      computed_hash = run_cmd(resolve_cmd, &rrc);
      if (rrc == 0 && computed_hash && computed_hash[0])
      {
         char *nl = strchr(computed_hash, '\n');
         if (nl)
            *nl = '\0';
         current_hash = computed_hash;
      }
      else
      {
         free(computed_hash);
         computed_hash = NULL;
         current_hash = expected_commit;
      }
   }
   else
   {
      computed_hash = verify_compute_file_hash(project_root); /* returns tree hash */
      current_hash = computed_hash;
   }

   if (!current_hash)
   {
      if (msg_buf)
         snprintf(msg_buf, msg_len, "could not compute current tree hash");
      return 0;
   }

   verify_state_entry_t entries[VERIFY_STATE_MAX];
   int nent = read_verify_entries(project_root, entries, VERIFY_STATE_MAX);
   int idx = find_verify_entry(entries, nent, current_hash);

   if (idx < 0)
   {
      if (msg_buf)
      {
         if (nent == 0)
            snprintf(msg_buf, msg_len, "no verification recorded. Run 'aimee git verify' first.");
         else
            snprintf(msg_buf, msg_len, "commit %.8s not verified. Run 'aimee git verify' first.",
                     current_hash);
      }
      free(computed_hash);
      return 0;
   }
   free(computed_hash);

   int failed_steps = entries[idx].failed;
   int total_steps = entries[idx].total;
   time_t stored_ts = entries[idx].ts;
   double age_min = difftime(time(NULL), stored_ts) / 60.0;

   if (failed_steps > 0 && total_steps > 0)
   {
      if (msg_buf)
         snprintf(msg_buf, msg_len,
                  "verification failed: %d/%d step(s) failed (%.0f minutes ago). "
                  "Run 'aimee git verify' and fix all failures before continuing.",
                  failed_steps, total_steps, age_min);
      return 0;
   }

   if (entries[idx].step_results[0])
   {
      for (int i = 0; i < cfg.count; i++)
      {
         int rc = -1;
         if (!step_result_lookup(entries[idx].step_results, cfg.steps[i].name, &rc) || rc != 0)
         {
            if (msg_buf)
               snprintf(msg_buf, msg_len, "step '%s' not verified. Run 'aimee git verify'.",
                        cfg.steps[i].name);
            return 0;
         }
      }
   }

   if (msg_buf)
      snprintf(msg_buf, msg_len, "verified (%.0f minutes ago)", age_min);
   return 1;
}

/* --- Background job system --- */

static int verify_max_parallel_threads(void)
{
   const char *env = getenv("AIMEE_VERIFY_PARALLEL");
   if (env && env[0])
   {
      char *end = NULL;
      long v = strtol(env, &end, 10);
      if (end != env && v > 0)
      {
         if (v > 4)
            v = 4;
         return (int)v;
      }
   }

   /* Verify steps commonly invoke build tools that fan out internally. Run
    * steps serially unless explicitly overridden so two sessions cannot
    * multiply into concurrent all-core builds and test waves. */
   return 1;
}

/* Dependency-aware verify state machine. Pool items never block on siblings;
 * finishing steps re-queue the coordinator or finalize the run. */
typedef struct
{
   int job_id; /* async: background job ID; 0 = sync */
   int is_async;
   verify_config_t cfg;
   verify_thread_ctx_t contexts[MAX_VERIFY_STEPS];
   int step_state[MAX_VERIFY_STEPS]; /* 0=pending 1=running 2=done */
   int remaining;
   char project_root[MAX_PATH_LEN];
   char session_id[SERVER_SESSION_ID_MAX];
   char *file_hash;
   int has_changes;
   verify_job_t *job;
   compute_pool_t *pool;
   int owns_pool;
   pthread_mutex_t mutex;
   pthread_cond_t cond;
   int done; /* set by finalize for sync path */
} verify_coord_state_t;

typedef struct
{
   verify_coord_state_t *state;
   int step_idx;
} verify_step_arg_t;

/* Forward declarations */
static void verify_coordinator_fn(void *arg);
static void verify_coord_finalize(verify_coord_state_t *state);

static int verify_pool_current_thread(compute_pool_t *pool)
{
   if (!pool)
      return 0;
   pthread_t self = pthread_self();
   for (int i = 0; i < pool->thread_count; i++)
      if (pthread_equal(self, pool->threads[i]))
         return 1;
   return 0;
}

static void *verify_pool_shutdown_thread(void *arg)
{
   compute_pool_t *pool = (compute_pool_t *)arg;
   compute_pool_unregister_secondary(pool);
   compute_pool_shutdown(pool);
   free(pool);
   return NULL;
}

static void verify_pool_shutdown_async(compute_pool_t *pool)
{
   if (!pool)
      return;

   pthread_t tid;
   if (pthread_create(&tid, NULL, verify_pool_shutdown_thread, pool) == 0)
   {
      pthread_detach(tid);
      return;
   }

   LOG_WARN("git.verify", "failed to start verify-pool shutdown thread");
   if (!verify_pool_current_thread(pool))
   {
      compute_pool_unregister_secondary(pool);
      compute_pool_shutdown(pool);
      free(pool);
   }
}

static int verify_coord_cancel_requested(verify_coord_state_t *state)
{
   return state && state->job && state->job->cancel_requested;
}

static void verify_mark_pending_cancelled(verify_coord_state_t *state)
{
   if (!state)
      return;
   for (int i = 0; i < state->cfg.count; i++)
   {
      if (state->step_state[i] == 0)
      {
         state->contexts[i].rc = -1;
         state->contexts[i].output = safe_strdup("verify: cancelled because session closed\n");
         state->step_state[i] = 2;
         state->remaining--;
      }
   }
}

static void verify_step_fn(void *arg)
{
   verify_step_arg_t *sa = (verify_step_arg_t *)arg;
   verify_coord_state_t *state = sa->state;
   int idx = sa->step_idx;
   free(sa);

   verify_thread_ctx_t *ctx = &state->contexts[idx];
   compute_pool_set_job(POOL_JOB_VERIFY, "step=%s", ctx->step->name);

   if (ctx->project_root[0])
      run_cmd_set_cwd(ctx->project_root);

   verify_run_step(ctx);

   compute_pool_clear_job();

   pthread_mutex_lock(&state->mutex);
   state->step_state[idx] = 2;
   state->remaining--;
   int remaining = state->remaining;
   /* Re-queue coordinator only if there are pending (state=0) steps; if all
    * remaining steps are already running they will call finalize themselves. */
   int need_coord = 0;
   for (int i = 0; i < state->cfg.count && !need_coord; i++)
      if (state->step_state[i] == 0)
         need_coord = 1;
   pthread_mutex_unlock(&state->mutex);

   if (remaining == 0)
      verify_coord_finalize(state);
   else if (need_coord)
   {
      if (compute_pool_submit(state->pool, verify_coordinator_fn, state) != 0)
         verify_coordinator_fn(state); /* queue full: run inline (fast) */
   }
}

static void verify_coordinator_fn(void *arg)
{
   verify_coord_state_t *state = (verify_coord_state_t *)arg;

   compute_pool_set_job(POOL_JOB_VERIFY, "coordinator");

   pthread_mutex_lock(&state->mutex);

   int any_running = 0;

   if (verify_coord_cancel_requested(state))
      verify_mark_pending_cancelled(state);

   for (int i = 0; i < state->cfg.count; i++)
   {
      if (state->step_state[i] == 2)
         continue; /* done */
      if (state->step_state[i] == 1)
      {
         any_running = 1;
         continue; /* already running */
      }

      /* Check dependency */
      if (state->cfg.steps[i].after[0])
      {
         int dep_pass = 0, dep_fail = 0, dep_found = 0;
         for (int j = 0; j < state->cfg.count; j++)
         {
            if (strcmp(state->cfg.steps[j].name, state->cfg.steps[i].after) == 0)
            {
               dep_found = 1;
               if (state->step_state[j] == 2)
               {
                  if (state->contexts[j].rc == 0)
                     dep_pass = 1;
                  else
                     dep_fail = 1;
               }
               else
               {
                  any_running = 1; /* dep still in progress */
               }
               break;
            }
         }
         if (!dep_found || dep_fail)
         {
            /* Unknown or failed dependency — cascade failure */
            state->contexts[i].rc = -1;
            state->step_state[i] = 2;
            state->remaining--;
            continue;
         }
         if (!dep_pass)
            continue; /* dep not done yet */
      }

      /* Step is ready: try to submit */
      verify_step_arg_t *sa = malloc(sizeof(*sa));
      if (!sa)
      {
         state->contexts[i].rc = -1;
         state->step_state[i] = 2;
         state->remaining--;
         continue;
      }
      sa->state = state;
      sa->step_idx = i;
      state->step_state[i] = 1;

      pthread_mutex_unlock(&state->mutex);
      int submitted = (compute_pool_submit(state->pool, verify_step_fn, sa) == 0);
      pthread_mutex_lock(&state->mutex);

      if (!submitted)
      {
         /* Queue full: run this ready step on the coordinator's worker.
          * Otherwise a verify can stall forever when the queue is full of
          * unrelated work and no verify step was actually submitted to
          * re-queue the coordinator later. */
         state->step_state[i] = 1;
         pthread_mutex_unlock(&state->mutex);
         compute_pool_clear_job();
         verify_step_fn(sa);
         return;
      }
      else
      {
         any_running = 1;
      }
   }

   /* Detect stuck dependencies: pending steps with no running work */
   if (!any_running && state->remaining > 0)
   {
      for (int i = 0; i < state->cfg.count; i++)
      {
         if (state->step_state[i] == 0)
         {
            fprintf(stderr, "verify: step '%s' has unsatisfied dep '%s'\n",
                    state->cfg.steps[i].name, state->cfg.steps[i].after);
            state->contexts[i].rc = -1;
            state->step_state[i] = 2;
            state->remaining--;
         }
      }
   }

   int remaining = state->remaining;
   pthread_mutex_unlock(&state->mutex);

   compute_pool_clear_job();

   if (remaining == 0)
      verify_coord_finalize(state);
}

static void verify_coord_finalize(verify_coord_state_t *state)
{
   const char *bg_root = state->project_root[0] ? state->project_root : NULL;

   if (state->is_async)
   {
      verify_job_t *job = verify_job_get(state->job_id);
      if (job)
      {
         dstr_t res;
         dstr_init(&res);
         int cancelled = verify_coord_cancel_requested(state);
         if (cancelled)
            dstr_append_str(&res, "cancelled: owning session closed\n\n");
         if (state->has_changes)
            dstr_append_str(&res, "warning: uncommitted changes in working tree\n\n");

         pthread_mutex_lock(&job->lock);
         job->total = state->cfg.count;
         for (int i = 0; i < state->cfg.count; i++)
         {
            if (state->contexts[i].rc == 0)
            {
               job->passed++;
               if (state->contexts[i].skipped && state->contexts[i].skip_reason[0])
                  dstr_appendf(&res, "[%d/%d] %s: SKIP (%s)\n", i + 1, state->cfg.count,
                               state->cfg.steps[i].name, state->contexts[i].skip_reason);
               else if (state->contexts[i].skipped)
                  dstr_appendf(&res, "[%d/%d] %s: PASS (cached)\n", i + 1, state->cfg.count,
                               state->cfg.steps[i].name);
               else
                  dstr_appendf(&res, "[%d/%d] %s: PASS (%.1fs)\n", i + 1, state->cfg.count,
                               state->cfg.steps[i].name, state->contexts[i].elapsed);
            }
            else
            {
               job->failed++;
               dstr_appendf(&res, "[%d/%d] %s: FAIL (exit %d, %.1fs)\n", i + 1, state->cfg.count,
                            state->cfg.steps[i].name, state->contexts[i].rc,
                            state->contexts[i].elapsed);
               if (state->contexts[i].output)
                  dstr_append_str(&res, state->contexts[i].output);
               dstr_append_char(&res, '\n');
            }
            free(state->contexts[i].output);
            state->contexts[i].output = NULL;
         }

         char step_res_buf[256];
         format_step_results(state->contexts, state->cfg.count, step_res_buf, sizeof(step_res_buf));
         if (cancelled)
         {
            dstr_append_str(&res, "\nverification cancelled; state not recorded\n");
         }
         else if (state->has_changes)
         {
            dstr_append_str(&res,
                            "\nwarning: uncommitted changes; verification state not recorded\n");
         }
         else if (state->file_hash)
         {
            char *commit_hash = verify_compute_commit_hash(bg_root);
            snprintf(job->file_hash, sizeof(job->file_hash), "%s",
                     (commit_hash && commit_hash[0]) ? commit_hash : state->file_hash);
            free(commit_hash);
            if (write_verify_state(bg_root, time(NULL), state->file_hash, job->failed, job->total,
                                   step_res_buf) != 0)
               dstr_append_str(&res, "\nwarning: could not record verify state\n");
         }
         else
         {
            dstr_append_str(&res, "\nwarning: could not compute file hash\n");
         }

         job->output = dstr_steal(&res);
         job->active = 2; /* finished */
         pthread_mutex_unlock(&job->lock);
      }

      free(state->file_hash);
      pthread_mutex_destroy(&state->mutex);
      pthread_cond_destroy(&state->cond);
      compute_pool_t *pool = state->owns_pool ? state->pool : NULL;
      free(state);
      verify_pool_shutdown_async(pool);
   }
   else
   {
      /* Sync path: signal the waiting ephemeral thread.  The waiter owns cleanup. */
      pthread_mutex_lock(&state->mutex);
      state->done = 1;
      pthread_cond_broadcast(&state->cond);
      pthread_mutex_unlock(&state->mutex);
   }
}

/* --- MCP tool handler --- */

cJSON *handle_git_verify(server_ctx_t *server_ctx, cJSON *args, const char *session_id)
{
   cJSON *jaction = cJSON_GetObjectItemCaseSensitive(args, "action");
   cJSON *jasync = cJSON_GetObjectItemCaseSensitive(args, "async");
   cJSON *jjob_id = cJSON_GetObjectItemCaseSensitive(args, "job_id");
   cJSON *jbase = cJSON_GetObjectItemCaseSensitive(args, "base");

   const char *action_str = (jaction && cJSON_IsString(jaction)) ? jaction->valuestring : "run";
   /* action=run defaults to async: a full verify run (build+tests+sanitizers+coverage+fuzz)
    * can take 10+ minutes.  Running synchronously blocks the MCP worker thread for that
    * entire duration, causing the MCP client to time out.  Pass async=false to force
    * synchronous execution (e.g. in scripts that already poll separately).
    * All other actions (check, status, env, conflicts, prepare-pr) remain synchronous. */
   int is_async;
   if (strcmp(action_str, "run") == 0)
      is_async = !(jasync && cJSON_IsFalse(jasync)); /* async unless explicitly false */
   else
      is_async = (jasync && cJSON_IsTrue(jasync)); /* others: sync unless explicitly async */

   /* The dispatch layer (dispatch_git_tool) already called mcp_chdir_git_root() which
    * read the 'path' arg, applied the session's worktree mapping, and set the
    * thread-local run_cmd CWD to the correct worktree.  resolve_verify_root(NULL, ...)
    * picks that up via run_cmd, so we get the worktree path even when the caller passed
    * path=<main-repo-root>. */
   char project_root[MAX_PATH_LEN] = "";
   const char *verify_root =
       (resolve_verify_root(NULL, project_root, sizeof(project_root)) == 0 && project_root[0])
           ? project_root
           : NULL;

   /* Cross-project scope gate. When the target is not the session's current
    * project and cross-project verify is disabled (default), do not run, gate,
    * or auto-generate config for it. status/conflicts/install-hook are explicit
    * inspection/setup actions and remain available. */
   int in_scope = verify_project_in_scope(verify_root);
   if (!in_scope && strcmp(action_str, "check") == 0)
      return mcp_text("PASS: cross-project verify disabled — repository is not the session's "
                      "current project (not gated). Enable with: aimee config set "
                      "verify_cross_project true");
   if (!in_scope && (strcmp(action_str, "run") == 0 || strcmp(action_str, "env") == 0 ||
                     strcmp(action_str, "prepare-pr") == 0))
      return mcp_text("skipped: cross-project verify disabled — this repository is not the "
                      "session's current project, so no project.yaml was generated and no steps "
                      "were run. Enable with: aimee config set verify_cross_project true");

   if (strcmp(action_str, "status") == 0)
   {
      if (!cJSON_IsNumber(jjob_id))
         return mcp_text("error: missing or invalid 'job_id' for action=status");
      verify_job_t *job = verify_job_get(jjob_id->valueint);
      if (!job)
         return mcp_text("error: job not found");

      pthread_mutex_lock(&job->lock);
      dstr_t res;
      dstr_init(&res);
      if (job->active == 1 && job->cancel_requested)
         dstr_appendf(&res, "Job #%d is cancelling...\n", job->id);
      else if (job->active == 1)
         dstr_appendf(&res, "Job #%d is still running...\n", job->id);
      else
         dstr_appendf(&res, "Job #%d finished: %d passed, %d failed\n\n", job->id, job->passed,
                      job->failed);

      if (job->output)
         dstr_append_str(&res, job->output);

      cJSON *r = mcp_text(dstr_cstr(&res));
      dstr_free(&res);
      pthread_mutex_unlock(&job->lock);
      return r;
   }

   if (strcmp(action_str, "check") == 0)
   {
      /* Optional: caller may supply the exact commit SHA to validate against
       * (used by the pre-push hook which knows the SHA of the ref being
       * pushed).  When absent, verify_check falls back to recomputing from
       * the current HEAD of project_root. */
      cJSON *jcommit = cJSON_GetObjectItemCaseSensitive(args, "commit");
      const char *expected_commit =
          (jcommit && cJSON_IsString(jcommit)) ? jcommit->valuestring : NULL;

      char msg[512];
      int ok = verify_check(verify_root, expected_commit, msg, sizeof(msg));
      dstr_t res;
      dstr_init(&res);
      dstr_appendf(&res, "%s: %s", ok ? "PASS" : "FAIL", msg);
      cJSON *r = mcp_text(dstr_cstr(&res));
      dstr_free(&res);
      return r;
   }

   if (strcmp(action_str, "conflicts") == 0)
   {
      char *report = verify_resolve_conflicts(NULL);
      cJSON *r = mcp_text(report);
      free(report);
      return r;
   }

   if (strcmp(action_str, "env") == 0)
   {
      verify_config_t cfg;
      verify_load_config(verify_root, &cfg);
      char *report = verify_check_env(&cfg);
      cJSON *r = mcp_text(report);
      free(report);
      return r;
   }

   if (strcmp(action_str, "prepare-pr") == 0)
   {
      char *report = verify_prepare_pr(
          verify_root, (jbase && cJSON_IsString(jbase)) ? jbase->valuestring : NULL);
      cJSON *r = mcp_text(report);
      free(report);
      return r;
   }

   if (strcmp(action_str, "install-hook") == 0)
   {
      int rc = verify_install_git_hook(verify_root);
      if (rc == 0)
         return mcp_text("ok: pre-push hook installed — terminal git push is now gated by "
                         "aimee verify. Bypass with: git push --no-verify");
      if (rc == -2)
         return mcp_text("skipped: an existing pre-push hook was found that was not installed by "
                         "aimee. Add the aimee verify check to it manually.");
      return mcp_text("error: could not install pre-push hook (git dir not found or I/O error)");
   }

   /* Default: run verification */
   verify_config_t cfg;
   if (verify_load_config(verify_root, &cfg) != 0)
      return mcp_text("error: no verify config available — auto-generation failed (no Makefile "
                      "found, or no recognized targets). Create "
                      "~/.config/aimee/projects/<project>/project.yaml manually.");

   if (is_async && server_ctx)
   {
      if (session_id && session_id[0] && verify_session_has_active_job(session_id))
         return mcp_text(
             "verify busy: session already has a running verification — wait for it to finish "
             "or cancel it first");
      int session_busy = 0;
      verify_job_t *job = verify_job_alloc_for_session(session_id, &session_busy);
      if (!job)
      {
         if (session_busy)
            return mcp_text(
                "verify busy: session already has a running verification — retry after it "
                "finishes or close the session to cancel it");
         return mcp_text("error: could not allocate background job (too many active)");
      }

      compute_pool_t *verify_pool = calloc(1, sizeof(*verify_pool));
      if (!verify_pool)
      {
         verify_job_release(job);
         return mcp_text("error: out of memory");
      }
      if (compute_pool_init(verify_pool, verify_max_parallel_threads()) != 0)
      {
         free(verify_pool);
         verify_job_release(job);
         return mcp_text("error: could not start verify worker pool");
      }
      compute_pool_register_secondary(verify_pool, "verify");

      verify_coord_state_t *coord = calloc(1, sizeof(verify_coord_state_t));
      if (!coord)
      {
         compute_pool_unregister_secondary(verify_pool);
         compute_pool_shutdown(verify_pool);
         free(verify_pool);
         verify_job_release(job);
         return mcp_text("error: out of memory");
      }

      coord->job_id = job->id;
      coord->is_async = 1;
      coord->job = job;
      coord->pool = verify_pool;
      coord->owns_pool = 1;
      coord->remaining = cfg.count;
      if (session_id && session_id[0])
         snprintf(coord->session_id, sizeof(coord->session_id), "%s", session_id);
      if (verify_root)
         snprintf(coord->project_root, sizeof(coord->project_root), "%s", verify_root);
      memcpy(&coord->cfg, &cfg, sizeof(verify_config_t));

      for (int i = 0; i < cfg.count; i++)
      {
         coord->contexts[i].step = &coord->cfg.steps[i];
         coord->contexts[i].index = i;
         coord->contexts[i].total = cfg.count;
         coord->contexts[i].cancel_requested = &job->cancel_requested;
         if (verify_root)
            snprintf(coord->contexts[i].project_root, sizeof(coord->contexts[i].project_root), "%s",
                     verify_root);
      }

      coord->has_changes = verify_worktree_has_changes(verify_root);
      coord->file_hash = verify_compute_file_hash(verify_root);

      /* Apply per-step result cache before submitting */
      if (!coord->has_changes && coord->file_hash && coord->file_hash[0] && cfg.count > 0)
      {
         verify_state_entry_t ents[VERIFY_STATE_MAX];
         int nent = read_verify_entries(verify_root, ents, VERIFY_STATE_MAX);
         int eidx = find_verify_entry(ents, nent, coord->file_hash);
         if (eidx >= 0 && ents[eidx].step_results[0])
         {
            for (int i = 0; i < cfg.count; i++)
            {
               int saved_rc = -1;
               if (step_result_lookup(ents[eidx].step_results, cfg.steps[i].name, &saved_rc) &&
                   saved_rc == 0)
               {
                  coord->contexts[i].rc = 0;
                  coord->contexts[i].skipped = 1;
                  coord->step_state[i] = 2;
                  coord->remaining--;
               }
            }
         }
      }
      verify_incremental_apply(verify_root, &coord->cfg, coord->contexts, coord->step_state,
                               &coord->remaining);

      pthread_mutex_init(&coord->mutex, NULL);
      pthread_cond_init(&coord->cond, NULL);

      if (coord->remaining == 0)
      {
         /* All steps cached — finalize without touching the pool */
         verify_coord_finalize(coord);
      }
      else if (compute_pool_submit(verify_pool, verify_coordinator_fn, coord) != 0)
      {
         free(coord->file_hash);
         pthread_mutex_destroy(&coord->mutex);
         pthread_cond_destroy(&coord->cond);
         compute_pool_unregister_secondary(verify_pool);
         compute_pool_shutdown(verify_pool);
         free(verify_pool);
         verify_job_release(job);
         return mcp_text("error: compute queue full — retry in a moment");
      }

      char buf[128];
      snprintf(buf, sizeof(buf),
               "Started background verification job #%d. Use git_verify action=status job_id=%d to "
               "poll results.",
               job->id, job->id);
      return mcp_text(buf);
   }

   /* Sync path (explicit async=false, or CLI with no server_ctx).
    * Always uses an ephemeral pool — never touches server_ctx->pool.
    * Sharing the server pool here would deadlock when all pool threads are
    * running delegates that are themselves waiting for this verify result. */
   if (server_ctx && session_id && session_id[0] && verify_session_has_active_job(session_id))
      return mcp_text(
          "verify busy: session already has a running verification — wait for it to finish "
          "or cancel it first");

   int has_changes = verify_worktree_has_changes(verify_root);
   volatile int sync_cancel_requested = 0;
   int sync_cancel_registered = verify_register_session_cancel(session_id, &sync_cancel_requested);
   if (sync_cancel_registered < 0)
   {
      return mcp_text("verify busy: session already has a running verification — retry after it "
                      "finishes or close the session to cancel it");
   }

   verify_thread_ctx_t contexts[MAX_VERIFY_STEPS];
   memset(contexts, 0, sizeof(contexts));

   for (int i = 0; i < cfg.count; i++)
   {
      contexts[i].step = &cfg.steps[i];
      contexts[i].index = i;
      contexts[i].total = cfg.count;
      contexts[i].rc = -1;
      contexts[i].elapsed = 0;
      contexts[i].output = NULL;
      contexts[i].cancel_requested = &sync_cancel_requested;
      if (verify_root && verify_root[0])
         snprintf(contexts[i].project_root, sizeof(contexts[i].project_root), "%s", verify_root);
   }

   char *file_hash = verify_compute_file_hash(verify_root);
   verify_run_waves_on_pool(NULL, &cfg, contexts, has_changes ? NULL : file_hash);
   if (sync_cancel_registered)
      verify_unregister_session_cancel(&sync_cancel_requested);

   dstr_t result;
   dstr_init(&result);
   int all_passed = 1;

   if (sync_cancel_requested)
      dstr_append_str(&result, "cancelled: owning session closed\n\n");
   if (has_changes)
      dstr_append_str(&result, "warning: uncommitted changes in working tree\n\n");

   for (int i = 0; i < cfg.count; i++)
   {
      if (contexts[i].rc == 0)
      {
         if (contexts[i].skipped)
         {
            if (contexts[i].skip_reason[0])
               dstr_appendf(&result, "[%d/%d] %s: SKIP (%s)\n", i + 1, cfg.count, cfg.steps[i].name,
                            contexts[i].skip_reason);
            else
               dstr_appendf(&result, "[%d/%d] %s: PASS (cached)\n", i + 1, cfg.count,
                            cfg.steps[i].name);
         }
         else
            dstr_appendf(&result, "[%d/%d] %s: PASS (%.1fs)\n", i + 1, cfg.count, cfg.steps[i].name,
                         contexts[i].elapsed);
      }
      else
      {
         dstr_appendf(&result, "[%d/%d] %s: FAIL (exit %d, %.1fs)\n", i + 1, cfg.count,
                      cfg.steps[i].name, contexts[i].rc, contexts[i].elapsed);
         if (contexts[i].output && contexts[i].output[0])
         {
            size_t out_len = strlen(contexts[i].output);
            const char *show = contexts[i].output;
            if (out_len > 8192)
               show = contexts[i].output + out_len - 8192;
            dstr_append_str(&result, show);
            dstr_append_char(&result, '\n');
         }
         all_passed = 0;
      }
      free(contexts[i].output);
   }

   int failed_count = 0;
   for (int i = 0; i < cfg.count; i++)
      if (contexts[i].rc != 0)
         failed_count++;

   char step_res_buf[256];
   format_step_results(contexts, cfg.count, step_res_buf, sizeof(step_res_buf));
   if (sync_cancel_requested)
   {
      dstr_append_str(&result, "\nverification cancelled; state not recorded");
      free(file_hash);
   }
   else if (has_changes)
   {
      dstr_append_str(&result, "\nwarning: uncommitted changes; verification state not recorded");
      free(file_hash);
   }
   else if (file_hash)
   {
      time_t now = time(NULL);
      char *commit_hash = verify_compute_commit_hash(verify_root);
      const char *display_hash = (commit_hash && commit_hash[0]) ? commit_hash : file_hash;
      if (write_verify_state(verify_root, now, file_hash, failed_count, cfg.count, step_res_buf) ==
          0)
      {
         if (all_passed)
            dstr_appendf(&result, "\nall %d steps passed -- verified (%s)", cfg.count,
                         display_hash);
         else
            dstr_appendf(&result, "\n%d/%d step(s) failed -- verified with failures (%s)",
                         failed_count, cfg.count, display_hash);
      }
      else
         dstr_append_str(&result, "\nwarning: could not record verify state");
      free(commit_hash);
      free(file_hash);
   }
   else
      dstr_append_str(&result, "\nwarning: could not compute file hash");

   cJSON *r = mcp_text(dstr_cstr(&result));
   dstr_free(&result);
   return r;
}

/* Branch ownership operations have moved to branch_ownership.c. */
