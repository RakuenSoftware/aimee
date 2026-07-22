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

#include "aimee_home.h"
#include "compute_pool.h"
#include "git_verify.h"
#include "git_verify_internal.h"
#include "git_verify_select.h"
#include "git_verify_jobs.h"
#include "aimee.h"
#include "config.h"
#include "util.h"
#include "dstr.h"
#include "log.h"
#include "mcp_git.h"
#include "platform_path.h"

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
            else if (strncmp(trimmed, "tier:", 5) == 0 && cfg->count > 0)
            {
               char *val = trimmed + 5;
               while (*val == ' ')
                  val++;
               snprintf(cfg->steps[cfg->count - 1].tier, MAX_STEP_NAME, "%s", val);
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
                verify_state_step_result_lookup(ents[eidx].step_results, cfg->steps[i].name,
                                                &saved_rc) &&
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
         if (!verify_state_step_result_lookup(entries[idx].step_results, cfg.steps[i].name, &rc) ||
             rc != 0)
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
/* verify_build_verdict is declared in git_verify_internal.h (exposed for the
 * structured-contract unit test). */

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
         /* Capture the structured verdict before the loop frees per-step output, so
          * action=status format=json can return it (the driver's async path). */
         {
            cJSON *v = verify_build_verdict(state->contexts, state->cfg.count, cancelled,
                                            state->has_changes);
            char *nv = cJSON_PrintUnformatted(v);
            cJSON_Delete(v);
            free(job->verdict_json);
            /* On a serialize OOM keep an explicit verdict rather than silently
             * downgrading a real result to "no-verdict-recorded". */
            job->verdict_json = nv ? nv
                                   : strdup("{\"schema_version\":1,\"verdict\":\"unavailable\","
                                            "\"reason\":\"serialize-oom\"}");
         }
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

/* --- structured (format=json) verdict --- */

static const char *verify_step_tier(const verify_step_t *s)
{
   return (s && s->tier[0]) ? s->tier : "mechanical";
}

/* Build a structured verdict object from completed step contexts. Caller owns the
 * returned cJSON. The pass/fail accounting mirrors the human text exactly, but in
 * a machine-stable shape the autonomous driver consumes. A no-step / no-config /
 * out-of-scope run never reaches this builder — those map to verdict "unavailable"
 * at the call site, kept DISTINCT from a real "passed" so an unconfigured repo is
 * never read as verified (the §1 false-pass this packet closes for the driver). */
cJSON *verify_build_verdict(const verify_thread_ctx_t *ctxs, int n, int cancelled, int has_changes)
{
   int passed = 0, failed = 0;
   for (int i = 0; i < n; i++)
   {
      if (ctxs[i].rc == 0)
         passed++;
      else
         failed++;
   }
   cJSON *root = cJSON_CreateObject();
   cJSON_AddNumberToObject(root, "schema_version", 1);
   const char *verdict = cancelled ? "unavailable" : (failed ? "failed" : "passed");
   const char *reason = cancelled ? "cancelled" : (failed ? "steps-failed" : "ok");
   cJSON_AddStringToObject(root, "verdict", verdict);
   cJSON_AddStringToObject(root, "reason", reason);
   cJSON_AddNumberToObject(root, "total", n);
   cJSON_AddNumberToObject(root, "passed", passed);
   cJSON_AddNumberToObject(root, "failed", failed);
   cJSON_AddBoolToObject(root, "has_uncommitted_changes", has_changes ? 1 : 0);
   cJSON *steps = cJSON_AddArrayToObject(root, "steps");
   for (int i = 0; i < n; i++)
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "name", ctxs[i].step ? ctxs[i].step->name : "");
      cJSON_AddStringToObject(s, "tier", verify_step_tier(ctxs[i].step));
      const char *status = (ctxs[i].rc == 0) ? (ctxs[i].skipped ? "skip" : "pass") : "fail";
      cJSON_AddStringToObject(s, "status", status);
      cJSON_AddNumberToObject(s, "exit", ctxs[i].rc);
      cJSON_AddNumberToObject(s, "seconds", ctxs[i].elapsed);
      if (ctxs[i].skipped && ctxs[i].skip_reason[0])
         cJSON_AddStringToObject(s, "skip_reason", ctxs[i].skip_reason);
      if (ctxs[i].rc != 0 && ctxs[i].output && ctxs[i].output[0])
      {
         size_t len = strlen(ctxs[i].output);
         const char *show = (len > 8192) ? ctxs[i].output + len - 8192 : ctxs[i].output;
         cJSON_AddStringToObject(s, "log", show); /* tail, matching the text path's cap */
      }
      cJSON_AddItemToArray(steps, s);
   }
   return root;
}

/* Wrap a structured verdict as the MCP text payload: the CLI/driver reads the
 * single text item as a JSON document. Consumes obj. */
static cJSON *verify_json_response(cJSON *obj)
{
   char *s = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   cJSON *r =
       mcp_text(s ? s : "{\"schema_version\":1,\"verdict\":\"unavailable\",\"reason\":\"oom\"}");
   free(s);
   return r;
}

/* A bare {verdict,reason[,job_id]} response for the states with no per-step detail
 * (unavailable / pending). */
static cJSON *verify_json_status(const char *verdict, const char *reason, int job_id)
{
   cJSON *o = cJSON_CreateObject();
   cJSON_AddNumberToObject(o, "schema_version", 1);
   cJSON_AddStringToObject(o, "verdict", verdict);
   cJSON_AddStringToObject(o, "reason", reason);
   if (job_id > 0)
      cJSON_AddNumberToObject(o, "job_id", job_id);
   return verify_json_response(o);
}

/* --- MCP tool handler --- */

cJSON *handle_git_verify(server_ctx_t *server_ctx, cJSON *args, const char *session_id)
{
   cJSON *jaction = cJSON_GetObjectItemCaseSensitive(args, "action");
   cJSON *jasync = cJSON_GetObjectItemCaseSensitive(args, "async");
   cJSON *jjob_id = cJSON_GetObjectItemCaseSensitive(args, "job_id");
   cJSON *jbase = cJSON_GetObjectItemCaseSensitive(args, "base");

   const char *action_str = (jaction && cJSON_IsString(jaction)) ? jaction->valuestring : "run";
   /* format=json: return a machine-stable structured verdict instead of the human
    * text, for the autonomous driver. The text path is byte-for-byte unchanged when
    * format!=json, and the freshness cache is written identically either way. */
   cJSON *jformat = cJSON_GetObjectItemCaseSensitive(args, "format");
   int json_out = (jformat && cJSON_IsString(jformat) && strcmp(jformat->valuestring, "json") == 0);
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
    * inspection/setup actions and remain available.
    *
    * force_in_scope: an IN-PROCESS caller that is authoritative about the target
    * (the workflow engine's implement gate, verifying a specific work-item
    * worktree) sets this. The scope gate exists to stop a CHAT delegate from
    * verifying an unrelated repo via the session's worktree mapping; it is wrong
    * for the wfe gate, which runs on the scheduler thread where session_id()
    * resolves to the run's chat session — a different project — and would wrongly
    * report every implement verify "out-of-scope" (observed live: every slice's
    * implement looped to its cap on an "unavailable" verdict). Not settable over
    * the wire tool schema — only the in-process provider passes it. */
   const cJSON *jforce = cJSON_GetObjectItemCaseSensitive(args, "force_in_scope");
   int in_scope = (jforce && cJSON_IsTrue(jforce)) ? 1 : verify_project_in_scope(verify_root);
   if (!in_scope && strcmp(action_str, "check") == 0)
   {
      if (json_out)
         return verify_json_status("unavailable", "out-of-scope", 0);
      return mcp_text("PASS: cross-project verify disabled — repository is not the session's "
                      "current project (not gated). Enable with: aimee config set "
                      "verify_cross_project true");
   }
   if (!in_scope && (strcmp(action_str, "run") == 0 || strcmp(action_str, "env") == 0 ||
                     strcmp(action_str, "prepare-pr") == 0))
   {
      if (json_out)
         return verify_json_status("unavailable", "out-of-scope", 0);
      return mcp_text("skipped: cross-project verify disabled — this repository is not the "
                      "session's current project, so no project.yaml was generated and no steps "
                      "were run. Enable with: aimee config set verify_cross_project true");
   }

   if (strcmp(action_str, "status") == 0)
   {
      if (!cJSON_IsNumber(jjob_id))
         return json_out ? verify_json_status("unavailable", "bad-job-id", 0)
                         : mcp_text("error: missing or invalid 'job_id' for action=status");
      verify_job_t *job = verify_job_get(jjob_id->valueint);
      if (!job)
         return json_out ? verify_json_status("unavailable", "job-not-found", 0)
                         : mcp_text("error: job not found");

      if (json_out)
      {
         pthread_mutex_lock(&job->lock);
         cJSON *r;
         if (job->active == 1) /* still running (cancelling counts as running) */
            r = verify_json_status("pending", "running", job->id);
         else if (job->verdict_json)
            r = mcp_text(job->verdict_json);
         else /* finished without a stored verdict (cancelled / hash failure) */
            r = verify_json_status("unavailable", "no-verdict-recorded", job->id);
         pthread_mutex_unlock(&job->lock);
         return r;
      }

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
      if (json_out)
      {
         /* verify_check returns 1 for BOTH "no verify section (no gate)" and
          * "fresh & valid". The driver must not read the former as a pass, so probe
          * the config first: no steps => unavailable/no-verify-section, distinct from
          * a real passed. Probing first also lets us skip verify_check's freshness
          * I/O entirely when there is no section. verify_load_config is idempotent
          * (it only (re)generates the same project.yaml), so the probe is safe. */
         verify_config_t cfg;
         if (verify_load_config(verify_root, &cfg) != 0 || cfg.count == 0)
            return verify_json_status("unavailable", "no-verify-section", 0);
         int ok = verify_check(verify_root, expected_commit, msg, sizeof(msg));
         return verify_json_status(ok ? "passed" : "failed", ok ? "ok" : "stale-or-failed", 0);
      }
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
   {
      if (json_out)
         return verify_json_status("unavailable", "no-verify-config", 0);
      return mcp_text("error: no verify config available — auto-generation failed (no Makefile "
                      "found, or no recognized targets). Create "
                      "~/.config/aimee/projects/<project>/project.yaml manually.");
   }

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
               if (verify_state_step_result_lookup(ents[eidx].step_results, cfg.steps[i].name,
                                                   &saved_rc) &&
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

      if (json_out)
         return verify_json_status("pending", "dispatched", job->id);
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

   /* Build the structured verdict BEFORE the text loop frees per-step output. The
    * text section still runs unconditionally so the freshness cache is written
    * identically whether the caller asked for json or text; only the RETURN
    * differs. */
   char *verdict_json_str = NULL;
   if (json_out)
   {
      cJSON *v = verify_build_verdict(contexts, cfg.count, sync_cancel_requested, has_changes);
      verdict_json_str = cJSON_PrintUnformatted(v);
      cJSON_Delete(v);
   }

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

   if (json_out)
   {
      dstr_free(&result);
      cJSON *r =
          mcp_text(verdict_json_str
                       ? verdict_json_str
                       : "{\"schema_version\":1,\"verdict\":\"unavailable\",\"reason\":\"oom\"}");
      free(verdict_json_str);
      return r;
   }
   cJSON *r = mcp_text(dstr_cstr(&result));
   dstr_free(&result);
   return r;
}

/* Branch ownership operations have moved to branch_ownership.c. */
