#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "aimee.h"
#include "agent_tools.h"
#include "cJSON.h"
#include "script_rpc.h"
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define SCRIPT_STDOUT_CAP (50 * 1024)
#define SCRIPT_STDERR_CAP (10 * 1024)

static int script_env_name_has_secret(const char *name)
{
   static const char *needles[] = {"KEY",        "TOKEN", "SECRET", "PASSWORD",
                                   "CREDENTIAL", "AUTH",  NULL};
   if (!name || !name[0])
      return 1;
   for (int i = 0; needles[i]; i++)
   {
      size_t nlen = strlen(needles[i]);
      for (const char *p = name; *p; p++)
      {
         size_t j = 0;
         while (j < nlen && p[j] && toupper((unsigned char)p[j]) == (unsigned char)needles[i][j])
            j++;
         if (j == nlen)
            return 1;
      }
   }
   return 0;
}

static int script_env_name_valid(const char *name)
{
   if (!name || !name[0] || script_env_name_has_secret(name))
      return 0;
   if (!(isalpha((unsigned char)name[0]) || name[0] == '_'))
      return 0;
   for (const char *p = name + 1; *p; p++)
      if (!(isalnum((unsigned char)*p) || *p == '_'))
         return 0;
   return 1;
}

static int script_env_name_allowed(const char *name)
{
   if (!name || !name[0])
      return 0;
   if (!script_env_name_valid(name))
      return 0;
   if (strcmp(name, "AIMEE_SESSION_ID") == 0 || strcmp(name, "AIMEE_TASK_ID") == 0)
      return 1;
   return strcmp(name, "PATH") == 0 || strcmp(name, "HOME") == 0 || strcmp(name, "USER") == 0 ||
          strcmp(name, "LANG") == 0 || strcmp(name, "TERM") == 0 || strncmp(name, "LC_", 3) == 0;
}

static int script_env_name_allowed_extra(const char *name)
{
   if (name && strncmp(name, "AIMEE_RPC_", 10) == 0)
      return 0;
   return script_env_name_valid(name);
}

static int script_env_add(char ***envp, int *count, int *cap, const char *name, const char *value)
{
   if (!envp || !count || !cap || !name || !value)
      return -1;
   size_t name_len = strlen(name);
   for (int i = 0; i < *count; i++)
   {
      if ((*envp)[i] && strncmp((*envp)[i], name, name_len) == 0 && (*envp)[i][name_len] == '=')
      {
         size_t len = name_len + strlen(value) + 2;
         char *entry = malloc(len);
         if (!entry)
            return -1;
         snprintf(entry, len, "%s=%s", name, value);
         free((*envp)[i]);
         (*envp)[i] = entry;
         return 0;
      }
   }
   if (*count + 2 >= *cap)
   {
      int ncap = *cap ? *cap * 2 : 32;
      char **grown = realloc(*envp, (size_t)ncap * sizeof(char *));
      if (!grown)
         return -1;
      *envp = grown;
      *cap = ncap;
   }
   size_t len = strlen(name) + strlen(value) + 2;
   char *entry = malloc(len);
   if (!entry)
      return -1;
   snprintf(entry, len, "%s=%s", name, value);
   (*envp)[(*count)++] = entry;
   (*envp)[*count] = NULL;
   return 0;
}

static void script_env_free(char **envp)
{
   if (!envp)
      return;
   for (int i = 0; envp[i]; i++)
      free(envp[i]);
   free(envp);
}

static char **script_env_build(const char *env_json)
{
   char **envp = NULL;
   int count = 0;
   int cap = 0;
   int has_path = 0;

   for (char **ep = environ; ep && *ep; ep++)
   {
      const char *eq = strchr(*ep, '=');
      if (!eq)
         continue;
      char name[128];
      size_t nlen = (size_t)(eq - *ep);
      if (nlen == 0 || nlen >= sizeof(name))
         continue;
      memcpy(name, *ep, nlen);
      name[nlen] = '\0';
      if (!script_env_name_allowed(name))
         continue;
      if (strcmp(name, "PATH") == 0)
         has_path = 1;
      if (script_env_add(&envp, &count, &cap, name, eq + 1) != 0)
      {
         script_env_free(envp);
         return NULL;
      }
   }

   if (!has_path &&
       script_env_add(&envp, &count, &cap, "PATH", "/usr/local/bin:/usr/bin:/bin") != 0)
   {
      script_env_free(envp);
      return NULL;
   }

   cJSON *extra = env_json && env_json[0] ? cJSON_Parse(env_json) : NULL;
   if (cJSON_IsObject(extra))
   {
      cJSON *item = NULL;
      cJSON_ArrayForEach(item, extra)
      {
         if (item->string && cJSON_IsString(item) && script_env_name_allowed_extra(item->string) &&
             script_env_add(&envp, &count, &cap, item->string, item->valuestring) != 0)
         {
            cJSON_Delete(extra);
            script_env_free(envp);
            return NULL;
         }
      }
   }
   cJSON_Delete(extra);

   if (!envp)
      envp = calloc(1, sizeof(char *));
   return envp;
}

static void script_read_pipe(int fd, char *buf, size_t cap, size_t *len, int *open_flag,
                             int *truncated)
{
   char tmp[4096];
   ssize_t n = read(fd, tmp, sizeof(tmp));
   if (n <= 0)
   {
      *open_flag = 0;
      close(fd);
      return;
   }
   size_t avail = *len < cap ? cap - *len : 0;
   if ((size_t)n > avail)
   {
      if (avail > 0)
      {
         memcpy(buf + *len, tmp, avail);
         *len += avail;
      }
      *truncated = 1;
      return;
   }
   memcpy(buf + *len, tmp, (size_t)n);
   *len += (size_t)n;
}

char *tool_execute_script(const char *language, const char *body, int timeout_secs,
                          const char *workdir, const char *env_json)
{
   if (!language || !body || !body[0])
      return safe_strdup("error: missing language or body");
   if (strcmp(language, "python") != 0 && strcmp(language, "bash") != 0)
      return safe_strdup("error: language must be 'python' or 'bash'");
   if (agent_tools_parent_write_guard_root())
      return safe_strdup("error: execute_script is blocked while the parent worktree is read-only");

   if (timeout_secs <= 0)
      timeout_secs = 120;
   if (timeout_secs > 600)
      timeout_secs = 600;

   int stdout_pipe[2], stderr_pipe[2];
   if (pipe(stdout_pipe) != 0)
      return safe_strdup("{\"stdout\":\"\",\"stderr\":\"pipe failed\",\"exit_code\":-1}");
   if (pipe(stderr_pipe) != 0)
   {
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      return safe_strdup("{\"stdout\":\"\",\"stderr\":\"pipe failed\",\"exit_code\":-1}");
   }

   char **envp = script_env_build(env_json);
   if (!envp)
   {
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      close(stderr_pipe[0]);
      close(stderr_pipe[1]);
      return safe_strdup("{\"stdout\":\"\",\"stderr\":\"env build failed\",\"exit_code\":-1}");
   }

   script_rpc_server_t rpc;
   if (script_rpc_prepare(&rpc) != 0)
   {
      script_rpc_cleanup(&rpc);
      script_env_free(envp);
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      close(stderr_pipe[0]);
      close(stderr_pipe[1]);
      return safe_strdup(
          "{\"stdout\":\"\",\"stderr\":\"script RPC setup failed\",\"exit_code\":-1}");
   }

   int count = 0;
   while (envp[count])
      count++;
   int cap = count + 1;
   char path_env[MAX_PATH_LEN * 2];
   const char *parent_path = getenv("PATH");
   snprintf(path_env, sizeof(path_env), "%s:%s", rpc.stub_dir,
            parent_path && parent_path[0] ? parent_path : "/usr/local/bin:/usr/bin:/bin");
   char pythonpath_env[MAX_PATH_LEN * 2];
   const char *parent_pythonpath = getenv("PYTHONPATH");
   snprintf(pythonpath_env, sizeof(pythonpath_env), "%s%s%s", rpc.stub_dir,
            parent_pythonpath && parent_pythonpath[0] ? ":" : "",
            parent_pythonpath && parent_pythonpath[0] ? parent_pythonpath : "");
   if (script_env_add(&envp, &count, &cap, "PATH", path_env) != 0 ||
       script_env_add(&envp, &count, &cap, "PYTHONPATH", pythonpath_env) != 0)
   {
      script_rpc_cleanup(&rpc);
      script_env_free(envp);
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      close(stderr_pipe[0]);
      close(stderr_pipe[1]);
      return safe_strdup("{\"stdout\":\"\",\"stderr\":\"script RPC env failed\",\"exit_code\":-1}");
   }
   if (rpc.client_fd >= 0)
   {
      char fd_env[32];
      snprintf(fd_env, sizeof(fd_env), "%d", rpc.client_fd);
      if (script_env_add(&envp, &count, &cap, "AIMEE_RPC_FD", fd_env) != 0)
      {
         script_rpc_cleanup(&rpc);
         script_env_free(envp);
         close(stdout_pipe[0]);
         close(stdout_pipe[1]);
         close(stderr_pipe[0]);
         close(stderr_pipe[1]);
         return safe_strdup(
             "{\"stdout\":\"\",\"stderr\":\"script RPC env failed\",\"exit_code\":-1}");
      }
   }

   struct timespec started;
   clock_gettime(CLOCK_MONOTONIC, &started);

   pid_t pid = fork();
   if (pid < 0)
   {
      script_rpc_cleanup(&rpc);
      script_env_free(envp);
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      close(stderr_pipe[0]);
      close(stderr_pipe[1]);
      return safe_strdup("{\"stdout\":\"\",\"stderr\":\"fork failed\",\"exit_code\":-1}");
   }

   if (pid == 0)
   {
      setpgid(0, 0);
      close(stdout_pipe[0]);
      close(stderr_pipe[0]);
      dup2(stdout_pipe[1], STDOUT_FILENO);
      dup2(stderr_pipe[1], STDERR_FILENO);
      close(stdout_pipe[1]);
      close(stderr_pipe[1]);

      const char *cwd = workdir && workdir[0] ? workdir : run_cmd_get_cwd();
      if (cwd && cwd[0] && chdir(cwd) != 0)
      {
         dprintf(STDERR_FILENO, "chdir failed: %s\n", cwd);
         _exit(127);
      }

      if (strcmp(language, "python") == 0)
      {
         char *const argv[] = {"python3", "-c", (char *)body, NULL};
         execvpe("python3", argv, envp);
      }
      else
      {
         char *const argv[] = {"bash", "-c", (char *)body, NULL};
         execvpe("bash", argv, envp);
      }
      _exit(127);
   }
   setpgid(pid, pid);
   if (rpc.client_fd >= 0)
   {
      close(rpc.client_fd);
      rpc.client_fd = -1;
   }
   if (script_rpc_start(&rpc) != 0)
   {
      script_rpc_cleanup(&rpc);
      script_env_free(envp);
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      close(stderr_pipe[0]);
      close(stderr_pipe[1]);
      kill(-pid, SIGKILL);
      waitpid(pid, NULL, 0);
      return safe_strdup(
          "{\"stdout\":\"\",\"stderr\":\"script RPC start failed\",\"exit_code\":-1}");
   }

   script_env_free(envp);
   close(stdout_pipe[1]);
   close(stderr_pipe[1]);

   char *out_buf = calloc(1, SCRIPT_STDOUT_CAP + 1);
   char *err_buf = calloc(1, SCRIPT_STDERR_CAP + 1);
   if (!out_buf || !err_buf)
   {
      free(out_buf);
      free(err_buf);
      close(stdout_pipe[0]);
      close(stderr_pipe[0]);
      kill(-pid, SIGKILL);
      waitpid(pid, NULL, 0);
      script_rpc_stop(&rpc);
      script_rpc_cleanup(&rpc);
      return safe_strdup("error: out of memory");
   }

   size_t out_len = 0, err_len = 0;
   int out_open = 1, err_open = 1, out_truncated = 0, err_truncated = 0, timed_out = 0;
   int max_fd = (stdout_pipe[0] > stderr_pipe[0] ? stdout_pipe[0] : stderr_pipe[0]) + 1;

   while (out_open || err_open)
   {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      long elapsed_ms =
          (now.tv_sec - started.tv_sec) * 1000 + (now.tv_nsec - started.tv_nsec) / 1000000;
      long remain_ms = (long)timeout_secs * 1000 - elapsed_ms;
      if (remain_ms <= 0)
      {
         timed_out = 1;
         break;
      }

      fd_set rfds;
      FD_ZERO(&rfds);
      if (out_open)
         FD_SET(stdout_pipe[0], &rfds);
      if (err_open)
         FD_SET(stderr_pipe[0], &rfds);
      struct timeval tv;
      tv.tv_sec = remain_ms / 1000;
      tv.tv_usec = (remain_ms % 1000) * 1000;
      int sel = select(max_fd, &rfds, NULL, NULL, &tv);
      if (sel < 0 && errno == EINTR)
         continue;
      if (sel <= 0)
      {
         timed_out = 1;
         break;
      }
      if (out_open && FD_ISSET(stdout_pipe[0], &rfds))
         script_read_pipe(stdout_pipe[0], out_buf, SCRIPT_STDOUT_CAP, &out_len, &out_open,
                          &out_truncated);
      if (err_open && FD_ISSET(stderr_pipe[0], &rfds))
         script_read_pipe(stderr_pipe[0], err_buf, SCRIPT_STDERR_CAP, &err_len, &err_open,
                          &err_truncated);
   }

   if (out_open)
      close(stdout_pipe[0]);
   if (err_open)
      close(stderr_pipe[0]);

   int exit_code = -1;
   if (timed_out)
   {
      kill(-pid, SIGTERM);
      usleep(100000);
      kill(-pid, SIGKILL);
      waitpid(pid, NULL, 0);
   }
   else
   {
      int status = 0;
      waitpid(pid, &status, 0);
      if (WIFEXITED(status))
         exit_code = WEXITSTATUS(status);
      else if (WIFSIGNALED(status))
         exit_code = 128 + WTERMSIG(status);
   }
   script_rpc_stop(&rpc);

   struct timespec ended;
   clock_gettime(CLOCK_MONOTONIC, &ended);
   long duration_ms =
       (ended.tv_sec - started.tv_sec) * 1000 + (ended.tv_nsec - started.tv_nsec) / 1000000;
   out_buf[out_len] = '\0';
   err_buf[err_len] = '\0';

   cJSON *result = cJSON_CreateObject();
   cJSON_AddStringToObject(result, "stdout", out_buf);
   cJSON_AddStringToObject(result, "stderr", err_buf);
   cJSON_AddNumberToObject(result, "exit_code", exit_code);
   cJSON_AddNumberToObject(result, "tool_calls", 0);
   cJSON_AddNumberToObject(result, "script_tool_calls", rpc.tool_calls);
   cJSON_AddNumberToObject(result, "script_policy_denials", rpc.policy_denials);
   cJSON_AddNumberToObject(result, "script_hmac_failures", rpc.hmac_failures);
   cJSON_AddNumberToObject(result, "duration_ms", duration_ms);
   cJSON_AddBoolToObject(result, "timed_out", timed_out);
   cJSON_AddBoolToObject(result, "stdout_truncated", out_truncated);
   cJSON_AddBoolToObject(result, "stderr_truncated", err_truncated);
   char *json = cJSON_PrintUnformatted(result);
   cJSON_Delete(result);
   free(out_buf);
   free(err_buf);
   script_rpc_cleanup(&rpc);
   return json ? json
               : safe_strdup("{\"stdout\":\"\",\"stderr\":\"json failed\",\"exit_code\":-1}");
}
