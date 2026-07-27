/* deploy_apply.c — server-orchestrated container deploy. See deploy_apply.h. */
#define _GNU_SOURCE 1

#include "deploy_apply.h"

#include "config.h"          /* config_t, config_load */
#include "config_database.h" /* config_emit_deploy_env */

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define DEPLOY_DEFAULT_COMPOSE "/opt/aimee/deploy/aimee-managed.compose.yaml"
#define DEPLOY_OUT_CAP         8192 /* tail of compose output kept for the UI */

/* Background-deploy state (one at a time; the wizard drives a single stack). */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_running = 0;
static int g_last_exit = INT_MIN;
static char g_last_out[DEPLOY_OUT_CAP];

int deploy_apply_enabled(void)
{
   const char *v = getenv("AIMEE_DEPLOY_ENABLED");
   return v && v[0] == '1' && v[1] == '\0';
}

void deploy_apply_compose_file(char *out, size_t cap)
{
   if (!out || cap == 0)
      return;
   const char *env = getenv("AIMEE_DEPLOY_COMPOSE_FILE");
   snprintf(out, cap, "%s", (env && env[0]) ? env : DEPLOY_DEFAULT_COMPOSE);
}

/* Free a NULL-terminated char* array (the merged env). */
static void free_envp(char **envp)
{
   if (!envp)
      return;
   for (size_t i = 0; envp[i]; i++)
      free(envp[i]);
   free(envp);
}

/* Build the child environment: the current environ, plus the deploy-env KEY=VALUE
 * lines config_emit_deploy_env produced for the live config (later entries win, so
 * the deploy-env overrides any inherited value). Returns a NULL-terminated,
 * heap-owned array, or NULL on OOM / no config. */
static char **build_deploy_envp(void)
{
   config_t cfg;
   if (config_load(&cfg) < 0)
      return NULL;
   char env[2048];
   config_emit_deploy_env(&cfg, env, sizeof(env));

   size_t base = 0;
   for (char **e = environ; e && *e; e++)
      base++;

   /* At most one added entry per line (lines are "KEY=VALUE\n"). */
   size_t extra = 0;
   for (const char *p = env; *p; p++)
      if (*p == '\n')
         extra++;

   char **envp = calloc(base + extra + 1, sizeof(char *));
   if (!envp)
      return NULL;
   size_t n = 0;
   for (char **e = environ; e && *e; e++)
   {
      envp[n] = strdup(*e);
      if (!envp[n])
      {
         free_envp(envp);
         return NULL;
      }
      n++;
   }
   /* Append each non-empty deploy-env line as its own entry. */
   const char *line = env;
   while (*line)
   {
      const char *nl = strchr(line, '\n');
      size_t len = nl ? (size_t)(nl - line) : strlen(line);
      if (len > 0)
      {
         char *entry = malloc(len + 1);
         if (!entry)
         {
            free_envp(envp);
            return NULL;
         }
         memcpy(entry, line, len);
         entry[len] = '\0';
         envp[n++] = entry;
      }
      if (!nl)
         break;
      line = nl + 1;
   }
   envp[n] = NULL;
   return envp;
}

/* Run `docker <argv...>` with the given environment, capturing combined
 * stdout+stderr into out (truncated to out_cap). *exit_code gets the child's exit
 * status (-1 if it did not exit normally). Returns 0 on success, -1 on
 * fork/pipe/wait failure. envp may be NULL (inherit environ). */
static int run_capture(const char *const argv[], char **envp, char *out, size_t out_cap,
                       int *exit_code)
{
   if (out && out_cap)
      out[0] = '\0';
   if (exit_code)
      *exit_code = -1;

   int pipefd[2];
   if (pipe(pipefd) != 0)
      return -1;

   pid_t pid = fork();
   if (pid < 0)
   {
      close(pipefd[0]);
      close(pipefd[1]);
      return -1;
   }
   if (pid == 0)
   {
      /* Child: combined stdout+stderr → the pipe write end. */
      close(pipefd[0]);
      dup2(pipefd[1], STDOUT_FILENO);
      dup2(pipefd[1], STDERR_FILENO);
      close(pipefd[1]);
      if (envp)
         execvpe("docker", (char *const *)argv, envp);
      else
         execvp("docker", (char *const *)argv);
      _exit(127);
   }

   close(pipefd[1]);
   size_t len = 0;
   char buf[1024];
   ssize_t r;
   while ((r = read(pipefd[0], buf, sizeof(buf))) > 0)
   {
      if (out && out_cap && len < out_cap - 1)
      {
         size_t room = (out_cap - 1) - len;
         size_t take = (size_t)r < room ? (size_t)r : room;
         memcpy(out + len, buf, take);
         len += take;
      }
      /* keep draining even once the buffer is full so the child never blocks */
   }
   close(pipefd[0]);
   if (out && out_cap)
      out[len] = '\0';

   int status = 0;
   if (waitpid(pid, &status, 0) != pid)
      return -1;
   if (exit_code)
      *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
   return 0;
}

/* Retire the pre-baked aimee-llm-cpu container left over from an older install.
 *
 * There is now ONE LLM service (aimee-llm, model-less, downloads the selected
 * tier). The retired aimee-llm-cpu carried the network alias `aimee-llm`, so a
 * leftover container would make that name resolve to two containers and the kb
 * could reach the stale one. It is no longer a service of the managed compose
 * file, so `up` will never touch it — it has to be removed by name.
 *
 * This cannot be done with --remove-orphans. The managed compose runs under the
 * SAME COMPOSE_PROJECT_NAME as compose.server-managed.yaml, so an orphan sweep
 * classifies aimee-server — not a service of the managed file — as an orphan and
 * stops and removes the very container running the deploy. (docker compose
 * --dry-run reports "Container aimee-aimee-server-1 Stopping/Removing".)
 *
 * Removing a container that was never up is a no-op, so failures here are not
 * fatal to the deploy; the output is appended for the wizard to show. */
static void deploy_retire_stale_llm(char **envp, const char *file, char *out, size_t out_cap)
{
   (void)file; /* the service is gone from the compose file; address it directly */
   const char *argv[] = {"docker", "rm", "-f", "aimee-aimee-llm-cpu-1", NULL};
   char buf[512];
   int code = -1;
   if (run_capture(argv, envp, buf, sizeof(buf), &code) == 0 && code == 0 && buf[0] &&
       out_cap > strlen(out) + 1)
      snprintf(out + strlen(out), out_cap - strlen(out), "retired legacy aimee-llm-cpu: %s", buf);
}

/* Background worker: `docker compose -f <file> up -d`.
 *
 * NO --remove-orphans, and this is not a style preference: it made the deploy
 * STOP THE SERVER RUNNING IT. aimee-server is started by compose.server-managed.yaml
 * under COMPOSE_PROJECT_NAME=aimee, and the managed file this command targets
 * defines only postgres/aimee-kb/aimee-llm. So compose finds a container in
 * project "aimee" that its file does not define, calls it an orphan, and removes
 * it — the orchestrator deleting itself mid-deploy. Observed on a clean install:
 * the wizard's Deploy step ran, and 47 seconds later the server logged
 * "server: shut down" and the container exited, leaving a new user with a dead
 * install and no obvious cause.
 *
 * The shared project name is deliberate (the managed services join the server's
 * network), so the fix is to drop the orphan sweep rather than the project. What
 * that gives up is small and recoverable: a service removed from the managed file
 * leaves its container behind until an operator prunes it. What it buys is that
 * deploying cannot destroy the thing doing the deploying.
 *
 * It also retires the LLM variant this deploy did NOT select (see
 * deploy_retire_stale_llm) — the one orphan the managed stack really can leave
 * behind, since the GPU and CPU services are mutually exclusive and both answer
 * to the network name `aimee-llm`. */
/* Fill argv with the managed-deploy `up` command for `file` and NULL-terminate it.
 * Deliberately omits --remove-orphans, for the reason above. Returns the number of
 * arguments written, or -1 when cap is too small. Separated out so the command is
 * assertable in a test rather than inlined in the worker. */
static int deploy_up_argv(const char *file, const char **argv, size_t cap)
{
   const char *cmd[] = {"docker", "compose", "-f", file, "up", "-d"};
   size_t n = sizeof(cmd) / sizeof(cmd[0]);
   if (!argv || cap < n + 1)
      return -1;
   for (size_t i = 0; i < n; i++)
      argv[i] = cmd[i];
   argv[n] = NULL;
   return (int)n;
}

static void *deploy_worker(void *arg)
{
   (void)arg;
   char file[512];
   deploy_apply_compose_file(file, sizeof(file));
   const char *argv[8];
   if (deploy_up_argv(file, argv, sizeof(argv) / sizeof(argv[0])) < 0)
   {
      pthread_mutex_lock(&g_lock);
      g_running = 0;
      g_last_exit = -1;
      snprintf(g_last_out, sizeof(g_last_out), "deploy: could not build the compose command\n");
      pthread_mutex_unlock(&g_lock);
      return NULL;
   }
   char **envp = build_deploy_envp();

   char out[DEPLOY_OUT_CAP];
   int code = -1;
   if (!envp)
      snprintf(out, sizeof(out), "deploy: failed to load config / build environment\n");
   else
   {
      out[0] = '\0';
      deploy_retire_stale_llm(envp, file, out, sizeof(out));
      size_t used = strlen(out);
      if (run_capture(argv, envp, out + used, sizeof(out) - used, &code) != 0)
         snprintf(out, sizeof(out),
                  "deploy: failed to run `docker compose` (is docker on PATH?)\n");
   }
   free_envp(envp);

   pthread_mutex_lock(&g_lock);
   g_running = 0;
   g_last_exit = code;
   snprintf(g_last_out, sizeof(g_last_out), "%s", out);
   pthread_mutex_unlock(&g_lock);
   return NULL;
}

int deploy_apply_start(void)
{
   pthread_mutex_lock(&g_lock);
   if (g_running)
   {
      pthread_mutex_unlock(&g_lock);
      return 1; /* already running */
   }
   g_running = 1;
   g_last_exit = INT_MIN;
   g_last_out[0] = '\0';
   pthread_mutex_unlock(&g_lock);

   pthread_t th;
   if (pthread_create(&th, NULL, deploy_worker, NULL) != 0)
   {
      pthread_mutex_lock(&g_lock);
      g_running = 0;
      pthread_mutex_unlock(&g_lock);
      return -1;
   }
   pthread_detach(th);
   return 0;
}

void deploy_apply_state(int *running, int *last_exit, char *out, size_t out_cap)
{
   pthread_mutex_lock(&g_lock);
   if (running)
      *running = g_running;
   if (last_exit)
      *last_exit = g_last_exit;
   if (out && out_cap)
      snprintf(out, out_cap, "%s", g_last_out);
   pthread_mutex_unlock(&g_lock);
}

int deploy_apply_status(char *out, size_t out_cap, int *exit_code)
{
   char file[512];
   deploy_apply_compose_file(file, sizeof(file));
   const char *argv[] = {"docker", "compose", "-f", file, "ps", "-a", "--format", "json", NULL};
   /* Pass the deploy env (COMPOSE_PROFILES + COMPOSE_PROJECT_NAME) so `ps` scopes
    * to the same project/profiles the apply used; fall back to environ on OOM. */
   char **envp = build_deploy_envp();
   int rc = run_capture(argv, envp, out, out_cap, exit_code);
   free_envp(envp);
   return rc;
}
