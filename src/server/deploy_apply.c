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

/* Background worker: `docker compose -f <file> up -d --remove-orphans`. */
static void *deploy_worker(void *arg)
{
   (void)arg;
   char file[512];
   deploy_apply_compose_file(file, sizeof(file));
   const char *argv[] = {"docker", "compose", "-f", file, "up", "-d", "--remove-orphans", NULL};
   char **envp = build_deploy_envp();

   char out[DEPLOY_OUT_CAP];
   int code = -1;
   if (!envp)
      snprintf(out, sizeof(out), "deploy: failed to load config / build environment\n");
   else if (run_capture(argv, envp, out, sizeof(out), &code) != 0)
      snprintf(out, sizeof(out), "deploy: failed to run `docker compose` (is docker on PATH?)\n");
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
