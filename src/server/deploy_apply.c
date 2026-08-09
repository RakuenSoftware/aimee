/* deploy_apply.c — server-orchestrated container deploy. See deploy_apply.h. */
#define _GNU_SOURCE 1

#include "deploy_apply.h"

#include "aimee_home.h"      /* one-shot legacy credential migration path */
#include "cJSON.h"           /* scope `docker compose ps` to the managed services */
#include "config.h"          /* config_t, config_load */
#include "config_database.h" /* config_emit_deploy_env */
#include "platform_random.h" /* 256-bit managed kb -> llm bearer */
#include "runtime_secret.h"
#include "vault_config_bootstrap.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define DEPLOY_DEFAULT_COMPOSE "/opt/aimee/deploy/aimee-managed.compose.yaml"
#define DEPLOY_OUT_CAP         8192                    /* tail of compose output kept for the UI */
#define DEPLOY_LLM_TOKEN_FILE  ".managed-kb-llm-token" /* legacy migration only */
#define DEPLOY_LLM_TOKEN_HEX   64                      /* 256-bit opaque bearer */

/* Background-deploy state (one at a time; the wizard drives a single stack). */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_running = 0;
static int g_last_exit = INT_MIN;
static char g_last_out[DEPLOY_OUT_CAP];

/* Is NAME= present in the emitted env with a non-empty value? config_emit_deploy_env
 * always emits the embedder keys, empty when unset, so presence alone proves nothing. */
static int deploy_env_value_set(const char *env, const char *name)
{
   if (!env || !name || !name[0])
      return 0;
   size_t nlen = strlen(name);
   for (const char *p = env; p && *p;)
   {
      const char *eol = strchr(p, '\n');
      if (!eol)
         eol = p + strlen(p);
      if ((size_t)(eol - p) > nlen && strncmp(p, name, nlen) == 0 && p[nlen] == '=')
      {
         const char *v = p + nlen + 1;
         while (v < eol && isspace((unsigned char)*v))
            v++;
         return v < eol;
      }
      p = (*eol == '\n') ? eol + 1 : eol;
   }
   return 0;
}

static int deploy_env_has_profile(const char *env, const char *profile)
{
   if (!profile || !profile[0])
      return 0;
   const char *p = env ? strstr(env, "COMPOSE_PROFILES=") : NULL;
   if (!p)
      return 0;
   p += strlen("COMPOSE_PROFILES=");
   const char *end = strchr(p, '\n');
   if (!end)
      end = p + strlen(p);
   while (p < end)
   {
      while (p < end && (*p == ',' || isspace((unsigned char)*p)))
         p++;
      const char *word = p;
      while (p < end && *p != ',' && !isspace((unsigned char)*p))
         p++;
      if ((size_t)(p - word) == strlen(profile) && memcmp(word, profile, strlen(profile)) == 0)
         return 1;
   }
   return 0;
}

static int deploy_llm_token_valid(const char *token)
{
   if (!token)
      return 0;
   size_t n = strlen(token);
   if (n < 32 || n > 512)
      return 0;
   /* RFC 6750 b64token alphabet. Generated values are lowercase hex; the wider
    * alphabet permits an explicitly supplied migration credential without
    * admitting whitespace/control bytes into an HTTP Authorization field. */
   for (size_t i = 0; i < n; i++)
      if (!(isalnum((unsigned char)token[i]) || token[i] == '-' || token[i] == '.' ||
            token[i] == '_' || token[i] == '~' || token[i] == '+' || token[i] == '/' ||
            token[i] == '='))
         return 0;
   return 1;
}

static void deploy_remove_legacy_token_file(const char *path, off_t size)
{
   int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
   if (fd >= 0)
   {
      char zeros[256] = {0};
      off_t left = size;
      while (left > 0)
      {
         size_t chunk = left > (off_t)sizeof(zeros) ? sizeof(zeros) : (size_t)left;
         if (write(fd, zeros, chunk) != (ssize_t)chunk)
            break;
         left -= (off_t)chunk;
      }
      (void)fsync(fd);
      close(fd);
   }
   (void)unlink(path);
}

/* Resolve or mint the managed KB-to-LLM service bearer. Vault is the only
 * durable authority; the old AIMEE_HOME file is consumed and erased once. */
static int deploy_llm_token(char *out, size_t cap)
{
   if (!out || cap < DEPLOY_LLM_TOKEN_HEX + 1)
      return -1;
   /* SYNTHESIS_API_KEY is the child credential and may be stale inherited
    * process state. It is deliberately ignored here. Operators who must adopt
    * an existing managed LLM use the distinct, explicit override variable. */
   char configured[513];
   if (runtime_secret_get("AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE", configured, sizeof(configured)))
   {
      if (!deploy_llm_token_valid(configured) || strlen(configured) >= cap)
      {
         runtime_secret_wipe(configured, sizeof(configured));
         return -1;
      }
      snprintf(out, cap, "%s", configured);
      int rc = vault_runtime_secret_set("SYNTHESIS_API_KEY", configured);
      runtime_secret_wipe(configured, sizeof(configured));
      return rc;
   }
   runtime_secret_wipe(configured, sizeof(configured));

   if (runtime_secret_get("SYNTHESIS_API_KEY", out, cap))
      return deploy_llm_token_valid(out) ? 0 : -1;

   const char *home = aimee_home();
   if (!home || !home[0])
      return -1;
   char path[PATH_MAX];
   int pn = snprintf(path, sizeof(path), "%s/%s", home, DEPLOY_LLM_TOKEN_FILE);
   if (pn < 0 || (size_t)pn >= sizeof(path))
      return -1;

   int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
   if (fd >= 0)
   {
      struct stat st;
      ssize_t n = read(fd, out, cap - 1);
      int saved = errno;
      int ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
               (st.st_mode & 077) == 0 && st.st_size > 0 && st.st_size < (off_t)cap &&
               n == st.st_size;
      close(fd);
      errno = saved;
      if (!ok)
         return -1;
      while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
         n--;
      out[n] = '\0';
      if (!deploy_llm_token_valid(out) || vault_runtime_secret_set("SYNTHESIS_API_KEY", out) != 0)
      {
         runtime_secret_wipe(out, cap);
         return -1;
      }
      deploy_remove_legacy_token_file(path, st.st_size);
      return 0;
   }
   if (errno != ENOENT)
      return -1;

   char proposed[DEPLOY_LLM_TOKEN_HEX + 1];
   if (platform_random_hex(proposed, DEPLOY_LLM_TOKEN_HEX) != 0)
      return -1;
   if (vault_runtime_secret_set("SYNTHESIS_API_KEY", proposed) != 0)
   {
      runtime_secret_wipe(proposed, sizeof(proposed));
      return -1;
   }
   snprintf(out, cap, "%s", proposed);
   runtime_secret_wipe(proposed, sizeof(proposed));
   return 0;
}

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

/* True when deploy_env contains a KEY= line for inherited KEY=VALUE. This makes
 * the documented "wizard config wins" merge real: duplicate process-environment
 * names have no portable first/last-wins rule. */
static int deploy_env_overrides(const char *deploy_env, const char *inherited)
{
   const char *eq = inherited ? strchr(inherited, '=') : NULL;
   if (!deploy_env || !eq)
      return 0;
   size_t key_len = (size_t)(eq - inherited);
   const char *line = deploy_env;
   while (*line)
   {
      const char *end = strchr(line, '\n');
      if (!end)
         end = line + strlen(line);
      if ((size_t)(end - line) > key_len && line[key_len] == '=' &&
          memcmp(line, inherited, key_len) == 0)
         return 1;
      line = *end ? end + 1 : end;
   }
   return 0;
}

/* Build the child environment: the current environ, plus the deploy-env KEY=VALUE
 * lines config_emit_deploy_env produced for the live config (later entries win, so
 * the deploy-env overrides any inherited value). Returns a NULL-terminated,
 * heap-owned array, or NULL on OOM / no config. */
/* Write `env` as the compose project's `.env`, atomically, best-effort.
 *
 * Only the non-secret topology reaches this file: config_emit_deploy_env omits
 * embedder_api_key and synthesis_api_key, and the managed-inference bearer is
 * appended to the child envp below rather than emitted, so it never lands on
 * disk. 0600 regardless, because the file still describes the deployment.
 *
 * Silent on failure and returns nothing: every caller has already obtained the
 * environment it needs directly, so there is no decision left to make here. */
static void deploy_write_compose_env_file(const char *env)
{
   if (!env || !env[0])
      return;
   const char *file = getenv("AIMEE_DEPLOY_COMPOSE_FILE");
   if (!file || !file[0])
      file = DEPLOY_DEFAULT_COMPOSE;

   const char *slash = strrchr(file, '/');
   if (!slash || slash == file)
      return;

   char tmp[PATH_MAX];
   char dest[PATH_MAX];
   const int dirlen = (int)(slash - file);
   if (snprintf(dest, sizeof(dest), "%.*s/.env", dirlen, file) >= (int)sizeof(dest))
      return;
   if (snprintf(tmp, sizeof(tmp), "%.*s/.env.tmp", dirlen, file) >= (int)sizeof(tmp))
      return;

   int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
   if (fd < 0)
      return;
   const size_t len = strlen(env);
   size_t off = 0;
   while (off < len)
   {
      ssize_t w = write(fd, env + off, len - off);
      if (w <= 0)
      {
         close(fd);
         unlink(tmp);
         return;
      }
      off += (size_t)w;
   }
   close(fd);
   if (rename(tmp, dest) != 0)
      unlink(tmp);
}

static char **build_deploy_envp(char *err, size_t err_cap, int *managed_llm_out,
                                int *managed_kb_out, int *managed_identity_out)
{
   if (err && err_cap)
      err[0] = '\0';
   if (managed_llm_out)
      *managed_llm_out = 0;
   if (managed_kb_out)
      *managed_kb_out = 0;
   if (managed_identity_out)
      *managed_identity_out = 0;
   if (!config_present())
   {
      if (err && err_cap)
         snprintf(err, err_cap, "could not load the saved wizard configuration");
      return NULL;
   }
   char env[2048];
   config_emit_deploy_env_current(env, sizeof(env));

   /* Keep the compose `.env` in step with the config this deploy is applying.
    *
    * The entrypoint writes it once at container start, which covers a restart or
    * an image swap. It does NOT cover config changed while this container keeps
    * running: the operator sets a new embedder, deploys from the UI, and the file
    * beside the compose still describes the old choice. A later manual
    * `docker compose up -d` would then quietly rebuild the old topology.
    *
    * Best-effort by design. The deploy child gets its environment from envp
    * below and does not read this file, so failing to refresh it must not fail
    * the deploy -- it only costs later callers their safety net. */
   deploy_write_compose_env_file(env);

   char llm_token[513] = "";
   /* Currently always 0: config_emit_deploy_env stopped emitting the "llm" profile when
    * the aimee-llm container was retired, so no managed inference service is deployed and
    * no KB-to-LLM credential is minted. The mechanism is kept (and still covered by
    * test_deploy_apply against a stub profile) because synthesis has not been resolved
    * yet — whatever serves it may well be a managed service again. */
   const int managed_llm = deploy_env_has_profile(env, "llm");
   const int managed_kb = deploy_env_has_profile(env, "kb");
   char explicit_conn[4096];
   int have_explicit_conn =
       runtime_secret_get("AIMEE_KB_CONN", explicit_conn, sizeof(explicit_conn));
   const char *explicit_id = getenv("AIMEE_SERVER_ID");
   const char *explicit_team = getenv("AIMEE_SERVER_TEAM_ID");
   const int explicit_parts = (have_explicit_conn ? 1 : 0) +
                              (explicit_id && explicit_id[0] ? 1 : 0) +
                              (explicit_team && explicit_team[0] ? 1 : 0);
   runtime_secret_wipe(explicit_conn, sizeof(explicit_conn));
   /* A managed kb with no embedder will start, print why it cannot serve retrieval, and
    * exit — leaving a failed deploy whose reason is a line in a container log. There is
    * no fallback to come up with instead, so refuse here, where the wizard shows it. */
   if (managed_kb && !deploy_env_value_set(env, "EMBEDDER_MODEL") &&
       !deploy_env_value_set(env, "EMBEDDER_URL"))
   {
      if (err && err_cap)
         snprintf(err, err_cap,
                  "no embedder selected: the knowledge base cannot serve retrieval without "
                  "one, and there is no fallback. Choose a bundled model on the wizard's "
                  "topology step (or `aimee config set embedder_model bekko-a25m`), or point "
                  "EMBEDDER_URL at an external endpoint, then deploy again");
      return NULL;
   }
   if (managed_kb && explicit_parts != 0 && explicit_parts != 3)
   {
      if (err && err_cap)
         snprintf(err, err_cap,
                  "partial explicit server identity: AIMEE_KB_CONN, AIMEE_SERVER_ID, and "
                  "AIMEE_SERVER_TEAM_ID must be set together");
      return NULL;
   }
   if (managed_llm_out)
      *managed_llm_out = managed_llm;
   if (managed_kb_out)
      *managed_kb_out = managed_kb;
   if (managed_identity_out)
      *managed_identity_out = managed_kb && explicit_parts == 0;
   if (managed_llm && deploy_llm_token(llm_token, sizeof(llm_token)) != 0)
   {
      if (err && err_cap)
         snprintf(err, err_cap,
                  "could not load or create the managed KB-to-LLM credential in Vault "
                  "(a legacy private file is accepted only for one-shot migration)");
      return NULL; /* fail closed: never launch a keyless managed LLM */
   }

   size_t base = 0;
   for (char **e = environ; e && *e; e++)
      base++;

   /* At most one added entry per line (lines are "KEY=VALUE\n"). */
   size_t extra = 0;
   for (const char *p = env; *p; p++)
      if (*p == '\n')
         extra++;

   char **envp = calloc(base + extra + (managed_llm ? 2 : 0) + 1, sizeof(char *));
   if (!envp)
   {
      if (err && err_cap)
         snprintf(err, err_cap, "out of memory while building the managed service environment");
      return NULL;
   }
   size_t n = 0;
   for (char **e = environ; e && *e; e++)
   {
      /* The generated/persisted credential is authoritative for this child.
       * Do not leave an inherited empty/stale duplicate before it: getenv and
       * Compose are not required to choose the later duplicate. */
      if (deploy_env_overrides(env, *e) ||
          /* sizeof-1, not a hand-counted length: these were literals matching the
           * OLD names, and a rename silently stopped the filter matching. */
          (managed_llm &&
           (strncmp(*e, "SYNTHESIS_API_KEY=", sizeof("SYNTHESIS_API_KEY=") - 1) == 0 ||
            strncmp(*e, "SYNTHESIS_AUTH_REQUIRED=", sizeof("SYNTHESIS_AUTH_REQUIRED=") - 1) == 0)))
         continue;
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
   if (managed_llm)
   {
      size_t len = strlen("SYNTHESIS_API_KEY=") + strlen(llm_token);
      envp[n] = malloc(len + 1);
      if (!envp[n])
      {
         free_envp(envp);
         return NULL;
      }
      snprintf(envp[n++], len + 1, "SYNTHESIS_API_KEY=%s", llm_token);
      memset(llm_token, 0, sizeof(llm_token));
      envp[n] = strdup("SYNTHESIS_AUTH_REQUIRED=1");
      if (!envp[n])
      {
         free_envp(envp);
         return NULL;
      }
      n++;
   }
   envp[n] = NULL;
   return envp;
}

/* Run `docker <argv...>` with the given environment, capturing combined
 * stdout+stderr into out (truncated to out_cap). *exit_code gets the child's exit
 * status (-1 if it did not exit normally). Returns 0 on success, -1 on
 * fork/pipe/wait failure. envp may be NULL (inherit environ). */
static int run_capture_input(const char *const argv[], char **envp, const void *input,
                             size_t input_len, char *out, size_t out_cap, int *exit_code)
{
   if (out && out_cap)
      out[0] = '\0';
   if (exit_code)
      *exit_code = -1;

   int pipefd[2];
   if (pipe(pipefd) != 0)
      return -1;
   int inputfd[2] = {-1, -1};
   if (input && (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, inputfd) != 0))
   {
      close(pipefd[0]);
      close(pipefd[1]);
      return -1;
   }

   pid_t pid = fork();
   if (pid < 0)
   {
      close(pipefd[0]);
      close(pipefd[1]);
      if (inputfd[0] >= 0)
      {
         close(inputfd[0]);
         close(inputfd[1]);
      }
      return -1;
   }
   if (pid == 0)
   {
      /* Child: combined stdout+stderr → the pipe write end. */
      close(pipefd[0]);
      dup2(pipefd[1], STDOUT_FILENO);
      dup2(pipefd[1], STDERR_FILENO);
      close(pipefd[1]);
      if (inputfd[0] >= 0)
      {
         close(inputfd[1]);
         dup2(inputfd[0], STDIN_FILENO);
         close(inputfd[0]);
      }
      if (envp)
         execvpe("docker", (char *const *)argv, envp);
      else
         execvp("docker", (char *const *)argv);
      _exit(127);
   }

   close(pipefd[1]);
   if (inputfd[0] >= 0)
   {
      close(inputfd[0]);
      const unsigned char *p = input;
      size_t sent = 0;
      while (sent < input_len)
      {
         ssize_t n = send(inputfd[1], p + sent, input_len - sent, MSG_NOSIGNAL);
         if (n <= 0)
            break;
         sent += (size_t)n;
      }
      close(inputfd[1]);
      if (sent != input_len)
      {
         close(pipefd[0]);
         (void)waitpid(pid, NULL, 0);
         return -1;
      }
   }
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

static int run_capture(const char *const argv[], char **envp, char *out, size_t out_cap,
                       int *exit_code)
{
   return run_capture_input(argv, envp, NULL, 0, out, out_cap, exit_code);
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
 * Removing a container that was never up is non-fatal. Docker's daemon error for
 * that expected case is deliberately not appended to the wizard output: it used
 * to put "retired legacy ... No such container" above the real compose failure,
 * making operators diagnose harmless cleanup instead of the actionable error. */

/* The container the retirement targets. Named, not derived, because it is no
 * longer a compose service — nothing can regenerate this string for us. */
#define DEPLOY_LEGACY_LLM_CPU_CONTAINER "aimee-aimee-llm-cpu-1"

/* Fill argv with the retirement command and NULL-terminate it. Separated out for
 * the same reason as deploy_up_argv: the command is then assertable in a test
 * without shelling out to a docker that may or may not exist on the machine
 * running the suite. Returns the argument count, or -1 when cap is too small. */
static int deploy_retire_argv(const char **argv, size_t cap)
{
   const char *cmd[] = {"docker", "rm", "-f", DEPLOY_LEGACY_LLM_CPU_CONTAINER};
   size_t n = sizeof(cmd) / sizeof(cmd[0]);
   if (!argv || cap < n + 1)
      return -1;
   for (size_t i = 0; i < n; i++)
      argv[i] = cmd[i];
   argv[n] = NULL;
   return (int)n;
}

static void deploy_retire_stale_llm(char **envp, const char *file, char *out, size_t out_cap)
{
   (void)file; /* the service is gone from the compose file; address it directly */
   const char *argv[8];
   if (deploy_retire_argv(argv, sizeof(argv) / sizeof(argv[0])) < 0)
      return;
   char buf[512];
   int code = -1;
   if (run_capture(argv, envp, buf, sizeof(buf), &code) == 0 && code == 0 &&
       out_cap > strlen(out) + 1)
      snprintf(out + strlen(out), out_cap - strlen(out),
               "retired obsolete aimee-llm-cpu container\n");
}

/* Background worker: ordered `docker compose -f <file> up -d --no-deps SERVICE`.
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
/* Fill argv with one ordered managed-service `up` command and NULL-terminate it.
 * --no-deps is deliberate: the orchestrator starts aimee-kb first so its CPU-only
 * initialization/indexing can run while aimee-llm downloads models. Deliberately
 * omits --remove-orphans, for the reason above. */
static int deploy_up_service_argv(const char *file, const char *service, const char **argv,
                                  size_t cap)
{
   const char *cmd[] = {"docker", "compose", "-f", file, "up", "-d", "--no-deps", service};
   size_t n = sizeof(cmd) / sizeof(cmd[0]);
   if (!argv || !file || !file[0] || !service || !service[0] || cap < n + 1)
      return -1;
   for (size_t i = 0; i < n; i++)
      argv[i] = cmd[i];
   argv[n] = NULL;
   return (int)n;
}

/* Bootstrap the managed KB's LLM service bearer over stdin before creating the
 * long-lived KB container. The one-shot is --rm and its Config.Env is clean;
 * only the pipe carries the first-boot value. */
static int deploy_kb_vault_bootstrap_argv(const char *file, const char **argv, size_t cap)
{
   const char *cmd[] = {"docker",       "compose",
                        "-f",           file,
                        "run",          "--rm",
                        "-T",           "--no-deps",
                        "--entrypoint", "/usr/local/bin/aimee-kb",
                        "aimee-kb",     "--bootstrap-vault-stdin"};
   size_t n = sizeof(cmd) / sizeof(cmd[0]);
   if (!argv || !file || !file[0] || cap < n + 1)
      return -1;
   for (size_t i = 0; i < n; i++)
      argv[i] = cmd[i];
   argv[n] = NULL;
   return (int)n;
}

static const char *deploy_env_value(char **envp, const char *name)
{
   size_t len = name ? strlen(name) : 0;
   for (size_t i = 0; envp && envp[i]; i++)
      if (strncmp(envp[i], name, len) == 0 && envp[i][len] == '=')
         return envp[i] + len + 1;
   return NULL;
}

/* Run the one-shot managed identity installer after the KB is healthy. The
 * service mounts both private named volumes and writes the server identity
 * directly, so no enrollment token or private key crosses host argv/stdout. */
static int deploy_identity_bootstrap_argv(const char *file, const char **argv, size_t cap)
{
   const char *cmd[] = {"docker", "compose", "-f", file,
                        "run",    "--rm",    "-T", "aimee-server-identity"};
   size_t n = sizeof(cmd) / sizeof(cmd[0]);
   if (!argv || !file || !file[0] || cap < n + 1)
      return -1;
   for (size_t i = 0; i < n; i++)
      argv[i] = cmd[i];
   argv[n] = NULL;
   return (int)n;
}

/* Run the isolated offline root/JWKS bootstrap. The service receives only named
 * volumes and the KB's private Unix socket; no custody key or trust bundle
 * crosses the server process, host argv, or captured output. */
static int deploy_authority_bootstrap_argv(const char *file, const char **argv, size_t cap)
{
   const char *cmd[] = {"docker", "compose", "-f", file,
                        "run",    "--rm",    "-T", "aimee-authority-bootstrap"};
   size_t n = sizeof(cmd) / sizeof(cmd[0]);
   if (!argv || !file || !file[0] || cap < n + 1)
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
   char env_err[256];
   int managed_llm = 0;
   int managed_kb = 0;
   int managed_identity = 0;
   char **envp =
       build_deploy_envp(env_err, sizeof(env_err), &managed_llm, &managed_kb, &managed_identity);

   char out[DEPLOY_OUT_CAP];
   int code = -1;
   if (!envp)
      snprintf(out, sizeof(out), "deploy: %s\n",
               env_err[0] ? env_err : "failed to build the managed service environment");
   else
   {
      out[0] = '\0';
      deploy_retire_stale_llm(envp, file, out, sizeof(out));
      code = 0;
      size_t used = 0;

      /* The server is already running this worker. Start KB next and LLM last.
       * Model downloads continue inside the LLM container after this deploy
       * finishes; KB initialization and CPU indexing do not wait for them. */
      if (managed_kb && managed_llm)
      {
         const char *token = deploy_env_value(envp, "SYNTHESIS_API_KEY");
         char record[sizeof("SYNTHESIS_API_KEY=") + 512];
         int record_len =
             token ? snprintf(record, sizeof(record), "SYNTHESIS_API_KEY=%s", token) : -1;
         const char *bootstrap_argv[16];
         int bootstrap_code = -1;
         used = strlen(out);
         if (record_len <= 0 || (size_t)record_len >= sizeof(record) ||
             deploy_kb_vault_bootstrap_argv(
                 file, bootstrap_argv, sizeof(bootstrap_argv) / sizeof(bootstrap_argv[0])) < 0 ||
             run_capture_input(bootstrap_argv, envp, record, (size_t)record_len + 1, out + used,
                               sizeof(out) - used, &bootstrap_code) != 0 ||
             bootstrap_code != 0)
         {
            code = bootstrap_code == 0 ? -1 : bootstrap_code;
            used = strlen(out);
            if (used < sizeof(out) - 1)
               snprintf(out + used, sizeof(out) - used,
                        "deploy: failed to seal the managed LLM credential into the KB Vault\n");
         }
         memset(record, 0, sizeof(record));
      }
      if (code == 0 && managed_kb)
      {
         const char *kb_argv[10];
         used = strlen(out);
         if (deploy_up_service_argv(file, "aimee-kb", kb_argv,
                                    sizeof(kb_argv) / sizeof(kb_argv[0])) < 0 ||
             run_capture(kb_argv, envp, out + used, sizeof(out) - used, &code) != 0)
         {
            code = -1;
            snprintf(out, sizeof(out), "deploy: failed to start aimee-kb with `docker compose`\n");
         }
      }
      if (code == 0 && managed_llm)
      {
         const char *llm_argv[10];
         used = strlen(out);
         if (deploy_up_service_argv(file, "aimee-llm", llm_argv,
                                    sizeof(llm_argv) / sizeof(llm_argv[0])) < 0 ||
             run_capture(llm_argv, envp, out + used, sizeof(out) - used, &code) != 0)
         {
            code = -1;
            used = strlen(out);
            if (used < sizeof(out) - 1)
               snprintf(out + used, sizeof(out) - used,
                        "deploy: failed to start aimee-llm with `docker compose`\n");
         }
      }
      if (code == 0 && !managed_kb && !managed_llm)
         snprintf(out + strlen(out), sizeof(out) - strlen(out),
                  "deploy: no managed sibling services selected\n");

      if (code == 0 && managed_identity)
      {
         const char *authority_argv[12];
         int authority_code = -1;
         char authority_out[1024] = "";
         if (deploy_authority_bootstrap_argv(
                 file, authority_argv, sizeof(authority_argv) / sizeof(authority_argv[0])) < 0 ||
             run_capture(authority_argv, envp, authority_out, sizeof(authority_out),
                         &authority_code) != 0 ||
             authority_code != 0)
         {
            code = authority_code == 0 ? -1 : authority_code;
            used = strlen(out);
            if (used < sizeof(out) - 1)
               snprintf(out + used, sizeof(out) - used,
                        "deploy: managed authority/JWKS bootstrap failed%s%s\n",
                        authority_out[0] ? ": " : "", authority_out);
         }
         else
         {
            used = strlen(out);
            if (used < sizeof(out) - 1)
               snprintf(out + used, sizeof(out) - used,
                        "deploy: managed authority roots and signed JWKS verified\n");
         }
      }
      if (code == 0 && managed_identity)
      {
         const char *identity_argv[12];
         int identity_code = -1;
         char identity_out[1024] = "";
         if (deploy_identity_bootstrap_argv(file, identity_argv,
                                            sizeof(identity_argv) / sizeof(identity_argv[0])) < 0 ||
             run_capture(identity_argv, envp, identity_out, sizeof(identity_out), &identity_code) !=
                 0 ||
             identity_code != 0)
         {
            code = identity_code == 0 ? -1 : identity_code;
            used = strlen(out);
            if (used < sizeof(out) - 1)
               snprintf(out + used, sizeof(out) - used,
                        "deploy: managed server identity enrollment failed%s%s\n",
                        identity_out[0] ? ": " : "", identity_out);
         }
         else
         {
            used = strlen(out);
            if (used < sizeof(out) - 1)
               snprintf(out + used, sizeof(out) - used,
                        "deploy: managed server identity enrolled and verified\n");
         }
      }
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

/* Keep only the containers the MANAGED compose file defines.
 *
 * `docker compose -f <managed file> ps` does NOT scope to the services in that
 * file — it scopes to COMPOSE_PROJECT_NAME. aimee-server is started from
 * compose.server-managed.yaml under the same project "aimee", so an unfiltered
 * `ps` reports the orchestrator alongside the services it manages. This is the
 * same project-vs-file blind spot documented for --remove-orphans above; there it
 * deleted the server, here it merely made a never-deployed install look deployed:
 * the wizard counts the returned services to label its button, so a clean box
 * offered "Re-deploy" for a knowledge base that did not exist yet.
 *
 * Filtering by the compose `project.config_files` label — rather than by passing
 * service names to `ps` — is what keeps this correct: every managed service sits
 * behind a profile (kb, identity-bootstrap, authority-bootstrap), so enumerating
 * them with `config --services` returns nothing unless the matching profiles are
 * active, and a profile-gated name passed to `ps` is an error rather than an
 * empty result. The label is present on every container regardless of profile.
 *
 * Emits a JSON array (a shape parse_ps already accepts). On any parse failure the
 * input is left untouched: a wrong service list is worse than an unfiltered one. */
static void deploy_filter_managed_ps(char *ps, size_t ps_cap, const char *file)
{
   const char *trimmed = ps;
   while (*trimmed == ' ' || *trimmed == '\n' || *trimmed == '\r' || *trimmed == '\t')
      trimmed++;
   if (!*trimmed)
      return;

   char want[600];
   snprintf(want, sizeof(want), "com.docker.compose.project.config_files=%s", file);

   cJSON *keep = cJSON_CreateArray();
   if (!keep)
      return;

   /* compose emits either a JSON array or newline-delimited objects. */
   int recognized = 0;
   cJSON *arr = cJSON_Parse(trimmed);
   if (arr && cJSON_IsArray(arr))
   {
      recognized = 1;
      cJSON *it = NULL;
      cJSON_ArrayForEach(it, arr)
      {
         const cJSON *l = cJSON_GetObjectItemCaseSensitive(it, "Labels");
         if (cJSON_IsString(l) && l->valuestring && strstr(l->valuestring, want))
            cJSON_AddItemToArray(keep, cJSON_Duplicate(it, 1));
      }
   }
   else
   {
      char *copy = strdup(trimmed);
      if (copy)
      {
         /* Recognized only once a line actually parses. Marking the shape valid
          * up front turns unparseable output into an empty array — reporting a
          * torn-down stack for one this code simply could not read. */
         for (char *p = copy; p && *p;)
         {
            char *nl = strchr(p, '\n');
            if (nl)
               *nl = '\0';
            if (*p)
            {
               cJSON *o = cJSON_Parse(p);
               if (o)
               {
                  recognized = 1;
                  const cJSON *l = cJSON_GetObjectItemCaseSensitive(o, "Labels");
                  if (cJSON_IsString(l) && l->valuestring && strstr(l->valuestring, want))
                     cJSON_AddItemToArray(keep, cJSON_Duplicate(o, 1));
                  cJSON_Delete(o);
               }
            }
            p = nl ? nl + 1 : NULL;
         }
         free(copy);
      }
   }
   cJSON_Delete(arr);

   if (recognized)
   {
      char *s = cJSON_PrintUnformatted(keep);
      /* Only replace when the result fits; a truncated array would not parse. */
      if (s && strlen(s) < ps_cap)
         snprintf(ps, ps_cap, "%s", s);
      free(s);
   }
   cJSON_Delete(keep);
}

int deploy_apply_status(char *out, size_t out_cap, int *exit_code)
{
   char file[512];
   deploy_apply_compose_file(file, sizeof(file));
   const char *argv[] = {"docker", "compose", "-f", file, "ps", "-a", "--format", "json", NULL};
   /* Pass the deploy env (COMPOSE_PROFILES + COMPOSE_PROJECT_NAME) so `ps` scopes
    * to the same project/profiles the apply used; fall back to environ on OOM. */
   char **envp = build_deploy_envp(NULL, 0, NULL, NULL, NULL);
   int rc = run_capture(argv, envp, out, out_cap, exit_code);
   free_envp(envp);
   if (rc == 0 && exit_code && *exit_code == 0)
      deploy_filter_managed_ps(out, out_cap, file);
   return rc;
}
