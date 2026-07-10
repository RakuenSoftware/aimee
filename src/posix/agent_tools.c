#include "aimee.h"
#include "util.h"
#include "agent_tools.h"
#include "aimee_home.h"
#include "tool_condense.h"
#include "log.h"
#include "agent_tools_internal.h"
#include "agent_source_authority.h"
#include "process_mgr.h"
#include "agent_exec.h"
#include "config.h"
#include "db1.h"
#include "sandbox.h"
#include "slop_detect.h"
#include "web_search.h"
#include "notes.h"
#include "kb.h"
#include "kb_client.h"
#include "mcp_client_registry.h"
#include "workspace.h"
#include "workspace_provider.h"
#include "diff.h"
#include "dstr.h"
#include "hashline_anchor.h"
#include "hashline_edit.h"
#include "guardrails_blast_radius.h"
#include "lsp.h"
#include "cJSON.h"
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <glob.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/wait.h>
int agent_get_durable_job_id(void) __attribute__((weak));
int db1_agent_job_is_cancelled(int job_id) __attribute__((weak));
int agent_delegation_stop_requested(char *buf, size_t bufsz) __attribute__((weak));
static int bash_delegate_cancel_requested(void)
{
   int job_id = agent_get_durable_job_id ? agent_get_durable_job_id() : 0;
   return (job_id > 0 && db1_agent_job_is_cancelled && db1_agent_job_is_cancelled(job_id)) ||
          (agent_delegation_stop_requested && agent_delegation_stop_requested(NULL, 0));
}
/* Single source of truth for the per-result MODEL-VISIBLE tool-output cap.
 * Reads tool_output_max_bytes from config (mtime-cached) and clamps it via the
 * header-inline agent_tool_output_cap_clamp(): 0/unset -> the built-in
 * AGENT_TOOL_OUTPUT_MAX default (32768); any positive value clamps to
 * (0, AGENT_TOOL_OUTPUT_RAW_MAX]. Callers resolve ONCE per call into a local so
 * the cap can't change mid-loop. */
size_t agent_tool_output_cap(void)
{
   config_t cfg;
   if (config_load(&cfg) != 0)
      return (size_t)AGENT_TOOL_OUTPUT_MAX;
   return agent_tool_output_cap_clamp(cfg.tool_output_max_bytes);
}
static void bash_kill_child_tree(pid_t pid)
{
   if (pid <= 0)
      return;
   kill(-pid, SIGTERM);
   kill(pid, SIGTERM);
   usleep(100000);
   kill(-pid, SIGKILL);
   kill(pid, SIGKILL);
}
static const char *bash_basename(const char *path)
{
   const char *slash = path ? strrchr(path, '/') : NULL;
   return slash ? slash + 1 : path;
}
static int bash_path_has_dir(const char *path_env, const char *dir)
{
   if (!path_env || !dir || !dir[0])
      return 0;
   size_t dir_len = strlen(dir);
   const char *p = path_env;
   while (*p)
   {
      const char *colon = strchr(p, ':');
      size_t len = colon ? (size_t)(colon - p) : strlen(p);
      if (len == dir_len && strncmp(p, dir, dir_len) == 0)
         return 1;
      if (!colon)
         break;
      p = colon + 1;
   }
   return 0;
}
static void bash_prepend_path_dir(char *buf, size_t buf_len, const char *dir)
{
   if (!buf || buf_len == 0 || !dir || !dir[0] || bash_path_has_dir(buf, dir))
      return;
   char candidate[MAX_PATH_LEN];
   int n = snprintf(candidate, sizeof(candidate), "%s/aimee", dir);
   if (n <= 0 || (size_t)n >= sizeof(candidate) || access(candidate, X_OK) != 0)
      return;
   char old[8192];
   snprintf(old, sizeof(old), "%s", buf);
   if (old[0])
      snprintf(buf, buf_len, "%s:%s", dir, old);
   else
      snprintf(buf, buf_len, "%s", dir);
}
static void bash_prepare_child_path(void)
{
   char path_buf[8192];
   const char *old_path = getenv("PATH");
   snprintf(path_buf, sizeof(path_buf), "%s", old_path ? old_path : "");
   char exe[MAX_PATH_LEN];
   ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
   if (n > 0)
   {
      exe[n] = '\0';
      char *slash = strrchr(exe, '/');
      if (slash && slash != exe)
      {
         *slash = '\0';
         bash_prepend_path_dir(path_buf, sizeof(path_buf), exe);
      }
   }
   const char *home = getenv("HOME");
   if (home && home[0])
   {
      char local_bin[MAX_PATH_LEN];
      int hn = snprintf(local_bin, sizeof(local_bin), "%s/.local/bin", home);
      if (hn > 0 && (size_t)hn < sizeof(local_bin))
         bash_prepend_path_dir(path_buf, sizeof(path_buf), local_bin);
   }
   bash_prepend_path_dir(path_buf, sizeof(path_buf), "/usr/local/bin");
   if (path_buf[0])
      setenv("PATH", path_buf, 1);
}
static int bash_word_in_list(const char *word, const char *const *list)
{
   if (!word)
      return 0;
   for (int i = 0; list[i]; i++)
   {
      if (strcmp(word, list[i]) == 0)
         return 1;
   }
   return 0;
}
static int bash_path_under_root(const char *path, const char *root)
{
   if (!path || !path[0] || !root || !root[0])
      return 0;
   size_t root_len = strlen(root);
   return strncmp(path, root, root_len) == 0 && (path[root_len] == '\0' || path[root_len] == '/');
}
static int bash_token_looks_path_like(const char *token)
{
   return token && token[0] &&
          (strchr(token, '/') || strcmp(token, ".") == 0 || strcmp(token, "..") == 0);
}
static int bash_path_under_guard_write_root(const char *path, const char *workspace)
{
   const char *write_root = agent_tools_parent_write_guard_write_root();
   if (!write_root || !write_root[0] || !path || !path[0])
      return 0;
   char resolved[MAX_PATH_LEN];
   normalize_path(path, workspace, resolved, sizeof(resolved));
   return bash_path_under_root(resolved, write_root);
}
static int bash_guarded_path_token_allowed(const char *token, const char *workspace)
{
   if (!token || !token[0])
      return 1;
   const char *value = strchr(token, '=');
   if (value && value[1] && bash_token_looks_path_like(value + 1) &&
       !bash_path_under_guard_write_root(value + 1, workspace))
      return 0;
   if (bash_token_looks_path_like(token) && !bash_path_under_guard_write_root(token, workspace))
      return 0;
   return 1;
}
static int bash_git_subcommand_is_readonly(char **tokens, int count)
{
   static const char *const readonly_git[] = {"status",     "diff",     "show",   "log",
                                              "rev-parse",  "ls-files", "branch", "describe",
                                              "merge-base", "grep",     "remote", NULL};
   const char *subcommand = NULL;
   for (int i = 1; i < count; i++)
   {
      if (strcmp(tokens[i], "-C") == 0 || strcmp(tokens[i], "-c") == 0)
      {
         i++;
         continue;
      }
      if (strncmp(tokens[i], "-C", 2) == 0 && tokens[i][2])
         continue;
      if (tokens[i][0] == '-')
         continue;
      subcommand = tokens[i];
      break;
   }
   return bash_word_in_list(subcommand, readonly_git);
}
static int bash_aimee_subcommand_is_readonly(char **tokens, int count)
{
   if (count < 2)
      return 0;
   const char *cmd = tokens[1];
   const char *sub = count >= 3 ? tokens[2] : "";
   if (strcmp(cmd, "index") == 0)
      return bash_word_in_list(sub, (const char *const[]){"overview", "list", "find", "structure",
                                                          "callers", "blast-radius", NULL});
   if (strcmp(cmd, "memory") == 0)
      return bash_word_in_list(sub, (const char *const[]){"search", "list", "get", "read", NULL});
   if (strcmp(cmd, "delegate") == 0)
      return bash_word_in_list(sub, (const char *const[]){"status", "log", "--list-roles", NULL});
   if (strcmp(cmd, "provider") == 0)
      return bash_word_in_list(sub, (const char *const[]){"list", "show", NULL});
   if (strcmp(cmd, "jobs") == 0 || strcmp(cmd, "job") == 0)
      return bash_word_in_list(sub, (const char *const[]){"list", "status", "logs", NULL});
   if (strcmp(cmd, "status") == 0 || strcmp(cmd, "workers") == 0)
      return 1;
   return 0;
}
static int bash_command_is_readonly_exec(char **tokens, int count, const char *workspace)
{
   static const char *const readonly_commands[] = {
       "cat",  "cut",    "dirname", "echo",  "file",     "find",     "grep",     "head", "ls",
       "nl",   "printf", "pwd",     "rg",    "sed",      "sort",     "stat",     "tail", "test",
       "true", "wc",     "uniq",    "false", "basename", "readlink", "realpath", NULL};
   if (count <= 0)
      return 0;
   for (int i = 0; i < count; i++)
   {
      if (util_token_is_shell_operator(tokens[i]))
         return 0;
   }
   const char *cmd = bash_basename(tokens[0]);
   if (strcmp(cmd, "cd") == 0)
      return count == 2 && bash_path_under_guard_write_root(tokens[1], workspace);
   if (strcmp(cmd, "git") == 0)
      return bash_git_subcommand_is_readonly(tokens, count);
   if (strcmp(cmd, "aimee") == 0)
      return bash_aimee_subcommand_is_readonly(tokens, count);
   if (!bash_word_in_list(cmd, readonly_commands))
      return 0;
   if (strcmp(cmd, "sed") == 0)
   {
      for (int i = 1; i < count; i++)
      {
         if (strcmp(tokens[i], "-i") == 0 || strncmp(tokens[i], "-i", 2) == 0)
            return 0;
      }
   }
   else if (strcmp(cmd, "find") == 0)
   {
      for (int i = 1; i < count; i++)
      {
         if (strcmp(tokens[i], "-delete") == 0 || strcmp(tokens[i], "-exec") == 0 ||
             strcmp(tokens[i], "-execdir") == 0 || strcmp(tokens[i], "-ok") == 0 ||
             strcmp(tokens[i], "-okdir") == 0 || strcmp(tokens[i], "-fprint") == 0 ||
             strcmp(tokens[i], "-fprintf") == 0)
            return 0;
      }
   }
   return 1;
}
static int bash_readonly_chain_validate(char **tokens, int count, int *command_starts,
                                        int *command_counts, int max_commands, char **operators,
                                        const char *workspace)
{
   if (!tokens || count <= 0 || !command_starts || !command_counts || !operators ||
       max_commands <= 0)
      return 0;
   int command_count = 0;
   int start = 0;
   operators[0] = NULL;
   for (int i = 0; i <= count; i++)
   {
      int at_end = i == count;
      int at_allowed_op = !at_end && (strcmp(tokens[i], "&&") == 0 || strcmp(tokens[i], ";") == 0 ||
                                      strcmp(tokens[i], "|") == 0);
      int at_blocked_op = !at_end && util_token_is_shell_operator(tokens[i]) && !at_allowed_op;
      if (at_blocked_op)
         return 0;
      if (!at_end && !at_allowed_op)
         continue;
      int segment_count = i - start;
      if (segment_count <= 0 || command_count >= max_commands)
         return 0;
      if (!bash_command_is_readonly_exec(tokens + start, segment_count, workspace))
         return 0;
      command_starts[command_count] = start;
      command_counts[command_count] = segment_count;
      command_count++;
      if (!at_end)
      {
         if (command_count >= max_commands)
            return 0;
         operators[command_count] = tokens[i];
         start = i + 1;
      }
   }
   return command_count;
}
static int bash_exec_readonly_pipeline(char **tokens, int *command_starts, int *command_counts,
                                       int first_command, int last_command, const char *workspace)
{
   int command_count = last_command - first_command + 1;
   int prev_read = -1;
   pid_t pids[32] = {0};
   for (int i = 0; i < command_count; i++)
   {
      int pipefd[2] = {-1, -1};
      int is_last = (i == command_count - 1);
      if (!is_last && pipe(pipefd) != 0)
      {
         if (prev_read >= 0)
            close(prev_read);
         return 127;
      }
      int idx = first_command + i;
      if (strcmp(tokens[command_starts[idx]], "cd") == 0)
      {
         if (prev_read >= 0)
            close(prev_read);
         if (pipefd[0] >= 0)
            close(pipefd[0]);
         if (pipefd[1] >= 0)
            close(pipefd[1]);
         return 127;
      }
      pid_t pid = fork();
      if (pid < 0)
      {
         if (prev_read >= 0)
            close(prev_read);
         if (pipefd[0] >= 0)
            close(pipefd[0]);
         if (pipefd[1] >= 0)
            close(pipefd[1]);
         return 127;
      }
      if (pid == 0)
      {
         if (prev_read >= 0)
         {
            dup2(prev_read, STDIN_FILENO);
            close(prev_read);
         }
         if (!is_last)
         {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            close(pipefd[1]);
         }
         if (workspace && workspace[0])
            (void)chdir(workspace);
         bash_prepare_child_path();
         const char *argv[65];
         for (int j = 0; j < command_counts[idx]; j++)
            argv[j] = tokens[command_starts[idx] + j];
         argv[command_counts[idx]] = NULL;
         execvp(argv[0], (char *const *)argv);
         _exit(127);
      }

      pids[i] = pid;
      if (prev_read >= 0)
         close(prev_read);
      if (!is_last)
      {
         close(pipefd[1]);
         prev_read = pipefd[0];
      }
   }

   int last_rc = 0;
   for (int i = 0; i < command_count; i++)
   {
      int status = 0;
      pid_t waited;
      while ((waited = waitpid(pids[i], &status, 0)) < 0 && errno == EINTR)
         ;
      if (waited < 0)
         last_rc = 127;
      else if (WIFEXITED(status))
         last_rc = WEXITSTATUS(status);
      else if (WIFSIGNALED(status))
         last_rc = 128 + WTERMSIG(status);
      else
         last_rc = 127;
   }
   return last_rc;
}

static int bash_exec_readonly_segment(char **tokens, int start, int count, const char *workspace)
{
   const char *argv[65];
   for (int i = 0; i < count; i++)
      argv[i] = tokens[start + i];
   argv[count] = NULL;

   pid_t pid = fork();
   if (pid < 0)
      return 127;
   if (pid == 0)
   {
      if (workspace && workspace[0])
         (void)chdir(workspace);
      bash_prepare_child_path();
      execvp(argv[0], (char *const *)argv);
      _exit(127);
   }

   int status = 0;
   pid_t waited;
   while ((waited = waitpid(pid, &status, 0)) < 0 && errno == EINTR)
      ;
   if (waited < 0)
      return 127;
   if (WIFEXITED(status))
      return WEXITSTATUS(status);
   if (WIFSIGNALED(status))
      return 128 + WTERMSIG(status);
   return 127;
}
static int bash_exec_readonly_cd(char **tokens, int start, int count, const char *workspace)
{
   if (count != 2 || strcmp(tokens[start], "cd") != 0 ||
       !bash_path_under_guard_write_root(tokens[start + 1], workspace))
      return -1;
   return chdir(tokens[start + 1]) == 0 ? 0 : 1;
}

static int bash_command_paths_stay_in_guarded_workspace(char **tokens, int count,
                                                        const char *workspace)
{
   for (int i = 0; i < count; i++)
   {
      if (!bash_guarded_path_token_allowed(tokens[i], workspace))
         return 0;
   }
   return 1;
}
static int bash_guarded_fallback_paths_safe(const char *command, const char *workspace)
{
   char *tokens[64] = {0};
   int count = shlex_split(command, tokens, 64);
   if (count <= 0 || count >= 64)
   {
      util_free_tokens(tokens, count > 0 ? count : 0);
      return 0;
   }
   int ok = bash_command_paths_stay_in_guarded_workspace(tokens, count, workspace);
   util_free_tokens(tokens, count);
   return ok;
}
static int bash_make_path_options_stay_in_workspace(char **tokens, int count, const char *workspace)
{
   for (int i = 1; i < count; i++)
   {
      const char *path = NULL;
      if (strcmp(tokens[i], "-C") == 0 || strcmp(tokens[i], "--directory") == 0 ||
          strcmp(tokens[i], "-f") == 0 || strcmp(tokens[i], "--file") == 0 ||
          strcmp(tokens[i], "--makefile") == 0)
      {
         if (i + 1 >= count)
            return 0;
         path = tokens[++i];
      }
      else if (strncmp(tokens[i], "-C", 2) == 0 && tokens[i][2])
         path = tokens[i] + 2;
      else if (strncmp(tokens[i], "-f", 2) == 0 && tokens[i][2])
         path = tokens[i] + 2;
      else if (strncmp(tokens[i], "--directory=", 12) == 0)
         path = tokens[i] + 12;
      else if (strncmp(tokens[i], "--file=", 7) == 0)
         path = tokens[i] + 7;
      else if (strncmp(tokens[i], "--makefile=", 11) == 0)
         path = tokens[i] + 11;

      if (path && !bash_path_under_guard_write_root(path, workspace))
         return 0;
   }
   return 1;
}

static int bash_command_is_guarded_workspace_segment(char **tokens, int count,
                                                     const char *workspace)
{
   if (count <= 0 || !workspace || !workspace[0])
      return 0;
   const char *write_root = agent_tools_parent_write_guard_write_root();
   if (!write_root || !bash_path_under_root(workspace, write_root))
      return 0;

   for (int i = 0; i < count; i++)
   {
      if (util_token_is_shell_operator(tokens[i]))
         return 0;
   }
   if (!bash_command_paths_stay_in_guarded_workspace(tokens, count, workspace))
      return 0;
   const char *cmd = bash_basename(tokens[0]);
   if (strcmp(cmd, "make") == 0)
      return bash_make_path_options_stay_in_workspace(tokens, count, workspace);
   if (strcmp(cmd, "mkdir") == 0 || strcmp(cmd, "touch") == 0 || strcmp(cmd, "cp") == 0 ||
       strcmp(cmd, "mv") == 0 || strcmp(cmd, "rm") == 0 || strcmp(cmd, "rmdir") == 0)
      return 1;
   if (strchr(tokens[0], '/'))
   {
      char resolved[MAX_PATH_LEN];
      normalize_path(tokens[0], workspace, resolved, sizeof(resolved));
      return bash_path_under_root(resolved, write_root) && access(resolved, X_OK) == 0;
   }
   return 0;
}

static int bash_workspace_chain_validate(char **tokens, int count, int *starts, int *counts,
                                         char **operators, int max_commands, const char *workspace)
{
   int n = 0, start = 0;
   operators[0] = NULL;
   for (int i = 0; i <= count; i++)
   {
      int end = i == count;
      int op = !end && (strcmp(tokens[i], "&&") == 0 || strcmp(tokens[i], ";") == 0);
      if (!end && util_token_is_shell_operator(tokens[i]) && !op)
         return 0;
      if (!end && !op)
         continue;
      if (i == start || n >= max_commands ||
          !bash_command_is_guarded_workspace_segment(tokens + start, i - start, workspace))
         return 0;
      starts[n] = start;
      counts[n++] = i - start;
      if (!op)
         continue;
      if (n >= max_commands)
         return 0;
      operators[n] = tokens[i];
      start = i + 1;
   }
   return n;
}

static pid_t guarded_readonly_exec(const char *command, int out_fd, int err_fd,
                                   const char *workspace, char *errbuf, size_t errbuf_len)
{
   char *tokens[64] = {0};
   int count = shlex_split(command, tokens, 64);
   if (count <= 0)
   {
      snprintf(errbuf, errbuf_len, "empty command");
      return -1;
   }
   int command_starts[32] = {0};
   int command_counts[32] = {0};
   char *operators[32] = {0};
   int readonly_chain = count < 64
                            ? bash_readonly_chain_validate(tokens, count, command_starts,
                                                           command_counts, 32, operators, workspace)
                            : 0;
   if (count >= 64 || readonly_chain <= 0)
   {
      util_free_tokens(tokens, count);
      snprintf(errbuf, errbuf_len, "command is not eligible for read-only direct execution");
      return -1;
   }

   pid_t pid = fork();
   if (pid == 0)
   {
      setpgid(0, 0);
      dup2(out_fd, STDOUT_FILENO);
      dup2(err_fd, STDERR_FILENO);
      bash_prepare_child_path();
      if (workspace && workspace[0])
         (void)chdir(workspace);

      if (readonly_chain == 1)
      {
         int start = command_starts[0];
         int n = command_counts[0];
         if (n == 2 && strcmp(tokens[start], "cd") == 0)
            _exit(bash_exec_readonly_cd(tokens, start, n, workspace));

         const char *argv[65];
         for (int j = 0; j < n; j++)
            argv[j] = tokens[start + j];
         argv[n] = NULL;
         execvp(argv[0], (char *const *)argv);
         _exit(127);
      }

      int last_rc = 0;
      for (int i = 0; i < readonly_chain; i++)
      {
         if (i > 0 && operators[i] && strcmp(operators[i], "&&") == 0 && last_rc != 0)
            continue;
         int pipe_end = i;
         while (pipe_end + 1 < readonly_chain && operators[pipe_end + 1] &&
                strcmp(operators[pipe_end + 1], "|") == 0)
            pipe_end++;
         if (pipe_end > i)
         {
            last_rc = bash_exec_readonly_pipeline(tokens, command_starts, command_counts, i,
                                                  pipe_end, workspace);
            i = pipe_end;
            continue;
         }

         last_rc = bash_exec_readonly_cd(tokens, command_starts[i], command_counts[i], workspace);
         if (last_rc < 0)
            last_rc =
                bash_exec_readonly_segment(tokens, command_starts[i], command_counts[i], NULL);
      }
      _exit(last_rc);
   }
   util_free_tokens(tokens, count);
   if (pid < 0)
      snprintf(errbuf, errbuf_len, "fork failed");
   return pid;
}

static pid_t guarded_workspace_exec(const char *command, int out_fd, int err_fd,
                                    const char *workspace, char *errbuf, size_t errbuf_len)
{
   char *tokens[64] = {0};
   int count = shlex_split(command, tokens, 64);
   if (count <= 0)
   {
      snprintf(errbuf, errbuf_len, "empty command");
      return -1;
   }
   int starts[32] = {0};
   int counts[32] = {0};
   char *operators[32] = {0};
   int chain = count < 64 ? bash_workspace_chain_validate(tokens, count, starts, counts, operators,
                                                          32, workspace)
                          : 0;
   if (chain <= 0)
   {
      util_free_tokens(tokens, count);
      snprintf(errbuf, errbuf_len, "command is not eligible for guarded workspace execution");
      return -1;
   }

   pid_t pid = fork();
   if (pid == 0)
   {
      setpgid(0, 0);
      dup2(out_fd, STDOUT_FILENO);
      dup2(err_fd, STDERR_FILENO);
      if (workspace && workspace[0])
         (void)chdir(workspace);
      bash_prepare_child_path();
      int last_rc = 0;
      for (int i = 0; i < chain; i++)
      {
         if (i > 0 && operators[i] && strcmp(operators[i], "&&") == 0 && last_rc != 0)
            continue;
         last_rc = bash_exec_readonly_segment(tokens, starts[i], counts[i], NULL);
      }
      _exit(last_rc);
   }
   util_free_tokens(tokens, count);
   if (pid < 0)
      snprintf(errbuf, errbuf_len, "fork failed");
   return pid;
}

static int lxc_cmd_safe(const char *cmd, const char *ro, const char *rw)
{
   return !agent_tools_cmd_refers_to_readonly_root(cmd, ro, rw);
}
int64_t auto_snapshot_record(const char *path)
{
   config_t cfg;
   if (config_load(&cfg) != 0 || !cfg.rewind_auto_snapshot)
      return 0;
   const char *sid = session_id();
   if (!sid || !sid[0])
      return 0;

   if (db1_init(cfg.db1_path) != 0)
      return 0;

   int64_t snap_id = agent_tools_get_snap_id();
   if (snap_id <= 0)
   {
      char label[64];
      int turn = agent_tools_get_turn();
      if (turn >= 0)
         snprintf(label, sizeof(label), "auto:turn%d", turn);
      else
         snprintf(label, sizeof(label), "auto");
      snap_id = db1_fsnap_get_or_create(sid, turn >= 0 ? turn : 0, label);
      agent_tools_set_snap_id(snap_id);
   }

   if (snap_id > 0)
      db1_fsnap_record_file(snap_id, path);

   return snap_id;
}

char *tool_bash(const char *command, int timeout_ms)
{
   /* Detached workspace (turn bound to a serving client): marshal the shell
    * command — with the thread-local cwd — over the reverse-channel so it runs
    * on the CLIENT's tree, not the server's fs. This is the agent-loop seam (the
    * file tools already route via workspace_provider_active(); only bash forked
    * locally). The local fork/exec + sandbox path below applies co-located. */
   const workspace_provider_t *ws = workspace_provider_active();
   if (ws && ws->kind == WS_PROVIDER_DETACHED && ws->exec_shell)
   {
      int exit_code = -1;
      char *out = ws->exec_shell(ws, command, &exit_code);
      cJSON *r = cJSON_CreateObject();
      cJSON_AddStringToObject(r, "stdout", out ? out : "");
      cJSON_AddStringToObject(r, "stderr", "");
      cJSON_AddNumberToObject(r, "exit_code", exit_code);
      free(out);
      char *res = cJSON_PrintUnformatted(r);
      cJSON_Delete(r);
      return res ? res : safe_strdup("{}");
   }

   int stdout_pipe[2], stderr_pipe[2];
   if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0)
      return safe_strdup("{\"stdout\":\"\",\"stderr\":\"pipe failed\",\"exit_code\":-1}");
   config_t cfg;
   sandbox_config_t sbox_cfg;
   memset(&sbox_cfg, 0, sizeof(sbox_cfg));
   if (config_load(&cfg) == 0)
      sbox_cfg = cfg.sandbox;
   const char *guard_ro = agent_tools_parent_write_guard_root();
   const char *guard_rw = agent_tools_parent_write_guard_write_root();
   int guarded_parent = guard_ro && guard_ro[0];
   char guarded_fallback_err[256] = "";
   pid_t pid;
#ifndef __linux__
   if (guarded_parent)
   {
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      close(stderr_pipe[0]);
      close(stderr_pipe[1]);
      return safe_strdup("{\"stdout\":\"\",\"stderr\":\"parent worktree write guard requires "
                         "Linux sandbox isolation for shell commands\",\"exit_code\":-1}");
   }
#endif
#ifdef __linux__
   if (guarded_parent)
   {
      sbox_cfg.mode = SANDBOX_MODE_ALLOWLIST;
      char cwd[MAX_PATH_LEN] = "";
      char workspace[MAX_PATH_LEN] = "";
      const char *workspace_ptr = NULL;
      const char *src = run_cmd_get_cwd();
      if (src && src[0])
         snprintf(cwd, sizeof(cwd), "%s", src);
      else if (!getcwd(cwd, sizeof(cwd)))
         cwd[0] = '\0';
      if (cwd[0] && guard_rw && bash_path_under_root(cwd, guard_rw))
         workspace_ptr = cwd;
      else if (cwd[0] && workspace_active_root(&cfg, cwd, workspace, sizeof(workspace)) == 0)
         workspace_ptr = workspace;
      else if (cwd[0])
         workspace_ptr = cwd;
      pid = guarded_readonly_exec(command, stdout_pipe[1], stderr_pipe[1], workspace_ptr,
                                  guarded_fallback_err, sizeof(guarded_fallback_err));
      if (pid < 0)
         pid = guarded_workspace_exec(command, stdout_pipe[1], stderr_pipe[1], workspace_ptr,
                                      guarded_fallback_err, sizeof(guarded_fallback_err));
      if (pid < 0)
         pid = sandbox_exec_with_readonly(&sbox_cfg, command, stdout_pipe[1], stderr_pipe[1],
                                          workspace_ptr, guard_ro, guard_rw);
      /* LXC fallback: no sandbox; allow plain exec if CWD and path-like args stay in-root. */
      if (pid < 0 && workspace_ptr && guard_rw && lxc_cmd_safe(command, guard_ro, guard_rw) &&
          bash_path_under_root(workspace_ptr, guard_rw) &&
          bash_guarded_fallback_paths_safe(command, workspace_ptr))
      {
         if ((pid = fork()) == 0)
         {
            setpgid(0, 0);
            dup2(stdout_pipe[1], STDOUT_FILENO);
            dup2(stderr_pipe[1], STDERR_FILENO);
            (void)chdir(workspace_ptr);
            bash_prepare_child_path();
            execl("/bin/sh", "sh", "-c", command, (char *)NULL);
            _exit(127);
         }
         if (pid > 0)
            guarded_fallback_err[0] = '\0';
      }
      close(stdout_pipe[1]);
      close(stderr_pipe[1]);
   }
   else if (sbox_cfg.mode != SANDBOX_MODE_OFF)
   {
      char cwd[MAX_PATH_LEN] = "";
      char workspace[MAX_PATH_LEN] = "";
      const char *workspace_ptr = NULL;
      const char *src = run_cmd_get_cwd();
      if (src && src[0])
         snprintf(cwd, sizeof(cwd), "%s", src);
      else if (!getcwd(cwd, sizeof(cwd)))
         cwd[0] = '\0';
      if (cwd[0] && workspace_active_root(&cfg, cwd, workspace, sizeof(workspace)) == 0)
         workspace_ptr = workspace;
      pid = sandbox_exec(&sbox_cfg, command, stdout_pipe[1], stderr_pipe[1], workspace_ptr);
      close(stdout_pipe[1]);
      close(stderr_pipe[1]);
   }
   else
   {
#endif /* __linux__ */
      pid = fork();
      if (pid < 0)
      {
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
         const char *child_cwd = run_cmd_get_cwd();
         if (child_cwd && child_cwd[0])
            (void)chdir(child_cwd);
         bash_prepare_child_path();
         execl("/bin/sh", "sh", "-c", command, (char *)NULL);
         _exit(127);
      }
      close(stdout_pipe[1]);
      close(stderr_pipe[1]);
#ifdef __linux__
   }
#endif /* __linux__ */
   if (pid < 0)
   {
      close(stdout_pipe[0]);
      close(stderr_pipe[0]);
      if (guarded_parent)
      {
         char msg[512];
         snprintf(msg, sizeof(msg),
                  "{\"stdout\":\"\",\"stderr\":\"parent worktree write guard requires "
                  "isolated shell execution; sandbox fallback could not start; %s\","
                  "\"exit_code\":-1}",
                  guarded_fallback_err[0] ? guarded_fallback_err
                                          : "direct fallback could not start");
         return safe_strdup(msg);
      }
      return safe_strdup("{\"stdout\":\"\",\"stderr\":\"fork failed\",\"exit_code\":-1}");
   }
   /* command-aware condensation (P1b): when reduce_command_filter is on, `rawcap` (the
    * MAXIMUM we'll capture) rises to the 2 MB ceiling so tool_condense sees the FULL output
    * (the old 32 KB read cap truncated the input before the lever could condense + spill it).
    * Default-off keeps the 32 KB cap — byte-identical. Per-call config load, reused for both
    * the cap and the condense step below (previously the load happened after allocation, so
    * it could not influence the cap). Buffers start SMALL and grow toward rawcap only if the
    * output actually overflows, so an `echo hi` costs ~32 KB, not 2 MB. */
   config_t tc_cfg;
   int tc_on = (config_load(&tc_cfg) == 0 && tool_condense_enabled(&tc_cfg));
   size_t rawcap = tc_on ? (size_t)TOOL_CONDENSE_CEILING : (size_t)AGENT_TOOL_OUTPUT_RAW_MAX;
   size_t out_cap =
       (rawcap < AGENT_TOOL_OUTPUT_RAW_MAX) ? rawcap : (size_t)AGENT_TOOL_OUTPUT_RAW_MAX;
   size_t err_cap = out_cap;
   char *out_buf = malloc(out_cap + 1);
   char *err_buf = malloc(err_cap + 1);
   if (!out_buf || !err_buf)
   {
      free(out_buf);
      free(err_buf);
      close(stdout_pipe[0]);
      close(stderr_pipe[0]);
      bash_kill_child_tree(pid);
      waitpid(pid, NULL, 0);
      return safe_strdup("error: out of memory");
   }
   size_t out_len = 0, err_len = 0;
   int timed_out = 0;
   struct timespec deadline;
   clock_gettime(CLOCK_MONOTONIC, &deadline);
   deadline.tv_sec += timeout_ms / 1000;
   deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
   if (deadline.tv_nsec >= 1000000000L)
   {
      deadline.tv_sec++;
      deadline.tv_nsec -= 1000000000L;
   }
   int max_fd = (stdout_pipe[0] > stderr_pipe[0] ? stdout_pipe[0] : stderr_pipe[0]) + 1;
   int stdout_open = 1, stderr_open = 1;
   int cancelled = 0;
   (void)setpgid(pid, pid);
   if (stdout_pipe[0] >= FD_SETSIZE || stderr_pipe[0] >= FD_SETSIZE)
   {
      close(stdout_pipe[0]);
      close(stderr_pipe[0]);
      bash_kill_child_tree(pid);
      waitpid(pid, NULL, 0);
      free(out_buf);
      free(err_buf);
      return safe_strdup("{\"stdout\":\"\",\"stderr\":\"fd exceeds FD_SETSIZE\",\"exit_code\":-1}");
   }
   while (stdout_open || stderr_open)
   {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      long remain_ms =
          (deadline.tv_sec - now.tv_sec) * 1000 + (deadline.tv_nsec - now.tv_nsec) / 1000000;
      if (remain_ms <= 0)
      {
         timed_out = 1;
         break;
      }
      if (bash_delegate_cancel_requested())
      {
         cancelled = 1;
         break;
      }
      fd_set rfds;
      FD_ZERO(&rfds);
      if (stdout_open)
         FD_SET(stdout_pipe[0], &rfds);
      if (stderr_open)
         FD_SET(stderr_pipe[0], &rfds);
      struct timeval tv;
      long poll_ms = remain_ms > 250 ? 250 : remain_ms;
      tv.tv_sec = poll_ms / 1000;
      tv.tv_usec = (poll_ms % 1000) * 1000;
      int sel = select(max_fd, &rfds, NULL, NULL, &tv);
      if (sel <= 0)
      {
         if (sel < 0 && errno == EINTR)
            continue;
         if (sel < 0)
         {
            timed_out = 1;
            break;
         }
         continue;
      }
      if (stdout_open && FD_ISSET(stdout_pipe[0], &rfds))
      {
         char discard[4096];
         /* grow toward rawcap only when the buffer actually filled (small outputs stay
          * cheap); a failed realloc just stops growing (we discard the excess, bounded). */
         if (out_len == out_cap && out_cap < rawcap)
         {
            size_t ncap = out_cap * 2 > rawcap ? rawcap : out_cap * 2;
            char *nb = realloc(out_buf, ncap + 1);
            if (nb)
            {
               out_buf = nb;
               out_cap = ncap;
            }
         }
         void *dst = out_len < out_cap ? out_buf + out_len : discard;
         size_t cap = out_len < out_cap ? out_cap - out_len : sizeof(discard);
         ssize_t n = read(stdout_pipe[0], dst, cap);
         if (n <= 0)
            stdout_open = 0;
         else if (out_len < out_cap)
            out_len += (size_t)n;
      }
      if (stderr_open && FD_ISSET(stderr_pipe[0], &rfds))
      {
         char discard[4096];
         if (err_len == err_cap && err_cap < rawcap)
         {
            size_t ncap = err_cap * 2 > rawcap ? rawcap : err_cap * 2;
            char *nb = realloc(err_buf, ncap + 1);
            if (nb)
            {
               err_buf = nb;
               err_cap = ncap;
            }
         }
         void *dst = err_len < err_cap ? err_buf + err_len : discard;
         size_t cap = err_len < err_cap ? err_cap - err_len : sizeof(discard);
         ssize_t n = read(stderr_pipe[0], dst, cap);
         if (n <= 0)
            stderr_open = 0;
         else if (err_len < err_cap)
            err_len += (size_t)n;
      }
   }
   if (stdout_pipe[0] >= 0)
      close(stdout_pipe[0]);
   if (stderr_pipe[0] >= 0)
      close(stderr_pipe[0]);
   int exit_code = -1;
   if (timed_out)
   {
      bash_kill_child_tree(pid);
      waitpid(pid, NULL, 0);
      exit_code = -1;
   }
   else if (cancelled)
   {
      bash_kill_child_tree(pid);
      waitpid(pid, NULL, 0);
      exit_code = -1;
   }
   else
   {
      int status = 0;
      waitpid(pid, &status, 0);
      if (WIFEXITED(status))
         exit_code = WEXITSTATUS(status);
   }
   out_buf[out_len] = '\0';
   err_buf[err_len] = '\0';

   /* Command-aware condensation (reduce_command_filter, default off): for a RECOGNIZED
    * command, deterministically condense the output (test failures kept, passes elided)
    * and spill the full raw so the delegate can read it back. Fail-open: any miss/decline
    * returns NULL and we fall through to the size-based agent_compress_tool_result. */
   char *cond_out = NULL, *cond_err = NULL;
   if (tc_on) /* reuse the config + gate resolved once at buffer-alloc (P1b) */
   {
      char spill_dir[3072];
      const char *home = aimee_home(); /* captured once; not called again before use */
      if (home && home[0] &&
          snprintf(spill_dir, sizeof spill_dir, "%s/tool-spills", home) < (int)sizeof spill_dir)
      {
         (void)mkdir(spill_dir, 0700); /* idempotent; 0700 per-user */
         tc_stats_t st;
         cond_out = tool_condense_apply(&tc_cfg, command, exit_code, out_buf, spill_dir, &st);
         if (cond_out)
            aimee_log(LOG_INFO, "tool_condense", "bash stdout condensed %ld->%ld (%s)",
                      st.raw_bytes, st.final_bytes, st.family);
         cond_err = tool_condense_apply(&tc_cfg, command, exit_code, err_buf, spill_dir, NULL);
      }
   }
   /* Compress output to fit token budget (#4) — the command-aware condensed form when we
    * produced one, else the size-based fallback. */
   char *compressed_out =
       cond_out ? cond_out : agent_compress_tool_result(out_buf, out_len, "bash");
   char *compressed_err =
       cond_err ? cond_err : agent_compress_tool_result(err_buf, err_len, "bash");

   /* Build JSON result */
   cJSON *result = cJSON_CreateObject();
   cJSON_AddStringToObject(result, "stdout", compressed_out);
   if (cancelled)
      cJSON_AddStringToObject(result, "stderr", "delegate cancelled during bash execution");
   else
      cJSON_AddStringToObject(result, "stderr", compressed_err);
   cJSON_AddNumberToObject(result, "exit_code", exit_code);
   char *json = cJSON_PrintUnformatted(result);
   cJSON_Delete(result);

   free(compressed_out);
   free(compressed_err);
   free(out_buf);
   free(err_buf);
   return json;
}

/* Validate a file path: delegates to the shared guardrail-level check. */
static const char *validate_file_path(const char *path, char *resolved, size_t resolved_len)
{
   return guardrails_validate_file_path(path, resolved, resolved_len);
}

const char *path_in_thread_cwd(const char *path, char *buf, size_t buf_len)
{
   if (!path || !path[0] || path[0] == '/')
      return path;
   /* A Windows-absolute client path (C:\... or C:/...) is already rooted — don't
    * prefix the detached turn's cwd onto it. */
   if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
       path[1] == ':' && (path[2] == '\\' || path[2] == '/'))
      return path;
   const char *cwd = run_cmd_get_cwd();
   if (!cwd || !cwd[0] || !buf || buf_len == 0)
      return path;
   snprintf(buf, buf_len, "%s/%s", cwd, path);
   return buf;
}

static int tool_buffer_looks_binary(const unsigned char *buf, size_t len)
{
   for (size_t i = 0; i < len; i++)
   {
      unsigned char c = buf[i];
      if (c == '\0' || c == 0x7f)
         return 1;
      if (c < 0x20 && c != '\n' && c != '\r' && c != '\t')
         return 1;
   }
   return 0;
}

static int tool_file_looks_binary(FILE *f)
{
   unsigned char sample[4096];
   long pos = ftell(f);
   if (pos < 0)
      pos = 0;
   size_t n = fread(sample, 1, sizeof(sample), f);
   int binary = tool_buffer_looks_binary(sample, n);
   if (fseek(f, pos, SEEK_SET) != 0)
      rewind(f);
   return binary;
}

/* Format the [offset,limit) window of `file_data` with composite `LINE:HASH| `
 * anchors, prefixed by a snapshot header. Lines are split on '\n' — the SAME
 * split hashline_snapshot_mint uses — so displayed ordinals and tags line up
 * with the snapshot's per-line digests exactly (the fgets/4096 raw loop can
 * split a long line, which would desync ordinals; the anchored path must not).
 * `buf` has capacity `cap`+1; returns bytes written into buf (NUL-terminated). */
static size_t format_anchored(char *buf, size_t cap, const char *file_data, size_t file_len,
                              int offset, int limit, const char *snap_id)
{
   size_t total = 0;
   char header[160];
   int hlen;
   if (snap_id)
      hlen =
          snprintf(header, sizeof(header),
                   "[anchored read — snapshot %s; edit each line by its N:hash anchor]\n", snap_id);
   else
      hlen = snprintf(header, sizeof(header),
                      "[anchored read — snapshot unavailable; re-read before editing by anchor]\n");
   if (hlen > 0 && (size_t)hlen < cap)
   {
      memcpy(buf, header, (size_t)hlen);
      total = (size_t)hlen;
   }

   /* Reserve room for a truncation marker so a cap hit is never silent. */
   static const char kTrunc[] =
       "[… output truncated at size cap; narrow with offset/limit, or raw:true for full bytes]\n";
   size_t trunc_len = sizeof(kTrunc) - 1;
   size_t body_cap = (cap > trunc_len) ? (cap - trunc_len) : 0;

   size_t i = 0;
   int line_num = 0;
   int lines_emitted = 0;
   int truncated = 0;
   int max_lines = (limit > 0) ? limit : 100000;
   while (i < file_len)
   {
      size_t s = i;
      while (i < file_len && file_data[i] != '\n')
         i++;
      size_t e = i;                /* content end, excludes '\n' */
      int has_nl = (i < file_len); /* a terminator follows */
      if (has_nl)
         i++;
      line_num++;
      if (offset > 0 && line_num <= offset)
         continue;

      uint64_t d = hashline_digest64(file_data + s, e - s, line_num == 1, has_nl);
      char tag[HASHLINE_DISPLAY_TAG_HEX + 1];
      hashline_display_tag(d, tag, sizeof(tag));
      char prefix[32];
      int plen = snprintf(prefix, sizeof(prefix), "%d:%s| ", line_num, tag);

      size_t content_len = e - s;
      size_t need = (size_t)plen + content_len + (has_nl ? 1u : 0u);
      if (total + need >= body_cap)
      {
         truncated = 1; /* drop an overflowing line rather than emit a malformed anchor */
         break;
      }
      memcpy(buf + total, prefix, (size_t)plen);
      total += (size_t)plen;
      memcpy(buf + total, file_data + s, content_len);
      total += content_len;
      if (has_nl)
         buf[total++] = '\n';

      lines_emitted++;
      if (lines_emitted >= max_lines)
      {
         /* Hit the internal safety cap on a no-limit read while lines remain:
          * mark it so the caller does not mistake a capped read for a full file.
          * A user-supplied limit reaching its count is expected, not truncation. */
         if (limit <= 0 && i < file_len)
            truncated = 1;
         break;
      }
   }
   if (truncated && total + trunc_len < cap)
   {
      memcpy(buf + total, kTrunc, trunc_len);
      total += trunc_len;
   }
   buf[total] = '\0';
   return total;
}

char *tool_read_file(const char *path, int offset, int limit)
{
   return tool_read_file_ex(path, offset, limit, 0, NULL);
}

char *tool_read_file_ex(const char *path, int offset, int limit, int anchored, const char *sid)
{
   const char *actual_path = path;
   char cwd_path[MAX_PATH_LEN];
   char proposal_buf[MAX_PATH_LEN];

   if (strncmp(path, "proposal:", 9) == 0)
   {
      char *resolved_proposal = resolve_proposal_path(path + 9);
      if (resolved_proposal)
      {
         /* Copy the resolved path into a function-lifetime buffer and free the
          * heap copy immediately, so nothing below aliases freed memory and there
          * is exactly one free with no per-return-path bookkeeping. */
         snprintf(proposal_buf, sizeof(proposal_buf), "%s", resolved_proposal);
         free(resolved_proposal);
         actual_path = proposal_buf;
      }
      else
         actual_path = path + 9; /* try as-is even if resolve failed */
   }
   actual_path = path_in_thread_cwd(actual_path, cwd_path, sizeof(cwd_path));

   char resolved[MAX_PATH_LEN];
   const char *err = validate_file_path(actual_path, resolved, sizeof(resolved));
   if (err)
      return safe_strdup(err);

   /* Pull the bytes through the workspace provider (shared = direct fs), then
    * run the existing binary-detection + line/offset/limit display loop over a
    * memory stream so its behavior is byte-for-byte unchanged. */
   const workspace_provider_t *ws = workspace_provider_active();
   char *file_data = NULL;
   size_t file_len = 0;
   if (ws->read_all(ws, actual_path, &file_data, &file_len) != 0)
   {
      char err_msg[512];
      snprintf(err_msg, sizeof(err_msg), "error: cannot open %s", actual_path);
      return safe_strdup(err_msg);
   }

   FILE *f = fmemopen(file_data, file_len, "rb");
   if (!f)
   {
      free(file_data);
      return safe_strdup("error: out of memory");
   }

   if (tool_file_looks_binary(f))
   {
      char err_msg[512];
      snprintf(err_msg, sizeof(err_msg), "error: binary file omitted: %s", actual_path);
      fclose(f);
      free(file_data);
      return safe_strdup(err_msg);
   }
   size_t cap = agent_tool_output_cap();
   char *buf = malloc(cap + 1);
   if (!buf)
   {
      fclose(f);
      free(file_data);
      return safe_strdup("error: out of memory");
   }

   /* Anchored path: mint a whole-file snapshot and emit LINE:HASH-prefixed lines
    * split on '\n' (consistent with the snapshot's per-line digests). */
   if (anchored)
   {
      fclose(f);
      char *snap = hashline_snapshot_mint(sid, actual_path, file_data, file_len);
      format_anchored(buf, cap, file_data, file_len, offset, limit, snap);
      free(snap);
      free(file_data);
      return buf;
   }

   size_t total = 0;
   char line[4096];
   int line_num = 0;
   int lines_read = 0;
   int max_lines = (limit > 0) ? limit : 100000;

   while (fgets(line, sizeof(line), f))
   {
      line_num++;
      if (offset > 0 && line_num <= offset)
         continue;
      size_t len = strlen(line);
      if (total + len >= cap)
      {
         size_t avail = cap - total;
         if (avail > 0)
            memcpy(buf + total, line, avail);
         total = cap;
         break;
      }
      memcpy(buf + total, line, len);
      total += len;
      lines_read++;
      if (lines_read >= max_lines)
         break;
   }
   fclose(f);
   free(file_data);
   buf[total] = '\0';
   return buf;
}

char *tool_write_file(const char *path, const char *content)
{
   char cwd_path[MAX_PATH_LEN];
   const char *actual_path = path_in_thread_cwd(path, cwd_path, sizeof(cwd_path));
   char resolved[MAX_PATH_LEN];
   const char *err = validate_file_path(actual_path, resolved, sizeof(resolved));
   if (err)
      return safe_strdup(err);
   if (agent_tools_readonly_delegate_blocks())
      return safe_strdup("error: write blocked: read-only delegate (not write-capable)");
   if (agent_tools_parent_write_guard_blocks(actual_path, NULL))
      return safe_strdup("error: write blocked: parent worktree is read-only for delegates");

   /* Route raw I/O through the workspace provider (shared = direct fs, the
    * same calls as before). Policy above this point — cwd resolution, path
    * validation, the parent-write guard — is unchanged. */
   const workspace_provider_t *ws = workspace_provider_active();

   char *old_content = NULL;
   {
      ws_stat_t st;
      ws->stat(ws, actual_path, &st);
      if (st.exists && st.size > 0 && st.size < 1024 * 1024)
      {
         size_t old_len = 0;
         if (ws->read_all(ws, actual_path, &old_content, &old_len) != 0)
            old_content = NULL;
      }
   }

   if (ws->write_all(ws, actual_path, content, content ? strlen(content) : 0) != 0)
   {
      free(old_content);
      char errbuf[512];
      snprintf(errbuf, sizeof(errbuf), "error: cannot write %s", actual_path);
      return safe_strdup(errbuf);
   }

   /* Compute and format structured diff */
   diff_result_t dr;
   if (diff_compute(old_content, content, &dr) == 0 && (dr.additions > 0 || dr.deletions > 0))
   {
      char *summary = diff_format_summary(&dr);
      char *unified = diff_format_unified(old_content, content, &dr);
      cJSON *payload = cJSON_CreateObject();
      cJSON_AddStringToObject(payload, "status", "ok");
      cJSON_AddStringToObject(payload, "path", actual_path);
      cJSON_AddBoolToObject(payload, "changed", 1);
      cJSON_AddStringToObject(payload, "summary", summary ? summary : "changed");
      cJSON_AddItemToObject(payload, "diff", diff_result_to_json(&dr));
      if (unified && unified[0])
         cJSON_AddStringToObject(payload, "unified_diff", unified);

      char *out = cJSON_PrintUnformatted(payload);
      cJSON_Delete(payload);
      free(old_content);
      free(summary);
      free(unified);
      if (out)
         return out;
      return safe_strdup("error: out of memory");
   }

   free(old_content);
   return safe_strdup("ok");
}

/* Parse an anchor token "N:tag" (or bare "N") into ordinal + display tag.
 * Returns 1 on success, 0 if malformed. `tag` is set to "" when no ":tag". */
static int hl_parse_anchor(const char *s, int *ord, char *tag, size_t tagsz)
{
   if (!s || !*s)
      return 0;
   char *end = NULL;
   long v = strtol(s, &end, 10);
   if (end == s || v < 1 || v > 1000000000)
      return 0;
   *ord = (int)v;
   tag[0] = '\0';
   if (*end == ':')
   {
      end++;
      size_t i = 0;
      while (*end && i + 1 < tagsz)
         tag[i++] = *end++;
      tag[i] = '\0';
   }
   return 1;
}

/* Build anchored context rows [start,end] (1-based, clamped) from `content` as a
 * cJSON array of {anchor:"N:hash", text:"..."} — the re-anchor payload lets the
 * model retry without a blind full re-read. */
static cJSON *hl_context_rows(const char *content, size_t len, int start, int end)
{
   cJSON *rows = cJSON_CreateArray();
   if (!rows)
      return NULL;
   size_t i = 0;
   int ord = 0;
   while (i < len)
   {
      size_t s = i;
      while (i < len && content[i] != '\n')
         i++;
      size_t e = i;
      int has_nl = (i < len);
      if (has_nl)
         i++;
      ord++;
      if (ord < start)
         continue;
      if (ord > end)
         break;
      uint64_t d = hashline_digest64(content + s, e - s, ord == 1, has_nl);
      char tag[HASHLINE_DISPLAY_TAG_HEX + 1];
      hashline_display_tag(d, tag, sizeof(tag));
      char anchor[32];
      snprintf(anchor, sizeof(anchor), "%d:%s", ord, tag);
      char *text = strndup(content + s, e - s);
      cJSON *row = cJSON_CreateObject();
      cJSON_AddStringToObject(row, "anchor", anchor);
      cJSON_AddStringToObject(row, "text", text ? text : "");
      cJSON_AddItemToArray(rows, row);
      free(text);
   }
   return rows;
}

char *tool_edit_file_anchored(const char *path, const char *snapshot_id, const cJSON *edits,
                              int dry_run, const char *sid)
{
   if (!path || !path[0])
      return safe_strdup("error: missing 'path' parameter");
   if (!snapshot_id || !snapshot_id[0])
      return safe_strdup("error: missing 'snapshot_id'; read the file (anchored) to obtain one");
   if (!edits || !cJSON_IsArray(edits) || cJSON_GetArraySize(edits) == 0)
      return safe_strdup("error: 'edits' must be a non-empty array");
   if (!dry_run && agent_tools_readonly_delegate_blocks())
      return safe_strdup("error: write blocked: read-only delegate (not write-capable)");

   char cwd_path[MAX_PATH_LEN];
   const char *actual_path = path_in_thread_cwd(path, cwd_path, sizeof(cwd_path));
   char resolved[MAX_PATH_LEN];
   const char *perr = validate_file_path(actual_path, resolved, sizeof(resolved));
   if (perr)
      return safe_strdup(perr);

   const workspace_provider_t *ws = workspace_provider_active();
   ws_stat_t stt;
   ws->stat(ws, actual_path, &stt);
   if (!stt.exists)
   {
      char e[512];
      snprintf(e, sizeof(e), "error: cannot open %s", actual_path);
      return safe_strdup(e);
   }
   if (stt.size >= 8 * 1024 * 1024)
      return safe_strdup("error: file too large to edit (limit 8MB); use write_file instead");
   char *content = NULL;
   size_t clen = 0;
   if (ws->read_all(ws, actual_path, &content, &clen) != 0)
   {
      char e[512];
      snprintf(e, sizeof(e), "error: cannot open %s", actual_path);
      return safe_strdup(e);
   }

   /* Resolve the read snapshot the anchors came from. */
   hashline_snapshot_view_t view;
   if (!hashline_snapshot_get(sid, snapshot_id, &view))
   {
      cJSON *p = cJSON_CreateObject();
      cJSON_AddStringToObject(p, "status", "stale_anchor");
      cJSON_AddStringToObject(p, "path", actual_path);
      cJSON_AddStringToObject(p, "reason", "snapshot_missing");
      cJSON_AddStringToObject(p, "hint",
                              "snapshot expired or unknown; re-read the file (anchored) to get "
                              "fresh anchors and a new snapshot_id, then retry");
      char *out = cJSON_PrintUnformatted(p);
      cJSON_Delete(p);
      free(content);
      return out ? out : safe_strdup("error: out of memory");
   }

   /* Parse edits into hl_edit_op_t[]. Text pointers borrow the cJSON strings. */
   int nops = cJSON_GetArraySize(edits);
   hl_edit_op_t *ops = calloc((size_t)nops, sizeof(hl_edit_op_t));
   if (!ops)
   {
      hashline_snapshot_view_free(&view);
      free(content);
      return safe_strdup("error: out of memory");
   }
   int parse_bad = -1;
   for (int k = 0; k < nops; k++)
   {
      cJSON *o = cJSON_GetArrayItem(edits, k);
      cJSON *opn = cJSON_GetObjectItem(o, "op");
      cJSON *at = cJSON_GetObjectItem(o, "at");
      cJSON *from = cJSON_GetObjectItem(o, "from");
      cJSON *to = cJSON_GetObjectItem(o, "to");
      cJSON *txt = cJSON_GetObjectItem(o, "text");
      const char *ops_s = (opn && cJSON_IsString(opn)) ? opn->valuestring : "";
      ops[k].text = (txt && cJSON_IsString(txt)) ? txt->valuestring : NULL;
      int a = 0, b = 0;
      if (strcmp(ops_s, "replace") == 0)
      {
         ops[k].kind = HL_OP_REPLACE;
         if (!at || !cJSON_IsString(at) ||
             !hl_parse_anchor(at->valuestring, &a, ops[k].from_tag, sizeof(ops[k].from_tag)))
         {
            parse_bad = k;
            break;
         }
         ops[k].from = ops[k].to = a;
      }
      else if (strcmp(ops_s, "insert_after") == 0)
      {
         ops[k].kind = HL_OP_INSERT_AFTER;
         if (!at || !cJSON_IsString(at) ||
             !hl_parse_anchor(at->valuestring, &a, ops[k].from_tag, sizeof(ops[k].from_tag)))
         {
            parse_bad = k;
            break;
         }
         ops[k].from = ops[k].to = a;
      }
      else if (strcmp(ops_s, "replace_range") == 0 || strcmp(ops_s, "delete_range") == 0)
      {
         ops[k].kind = (ops_s[0] == 'r') ? HL_OP_REPLACE_RANGE : HL_OP_DELETE_RANGE;
         if (!from || !cJSON_IsString(from) || !to || !cJSON_IsString(to) ||
             !hl_parse_anchor(from->valuestring, &a, ops[k].from_tag, sizeof(ops[k].from_tag)) ||
             !hl_parse_anchor(to->valuestring, &b, ops[k].to_tag, sizeof(ops[k].to_tag)))
         {
            parse_bad = k;
            break;
         }
         ops[k].from = a;
         ops[k].to = b;
      }
      else
      {
         parse_bad = k;
         break;
      }
   }
   if (parse_bad >= 0)
   {
      free(ops);
      hashline_snapshot_view_free(&view);
      free(content);
      char e[160];
      snprintf(e, sizeof(e),
               "error: edits[%d] is malformed (op/op-anchor); expected op in "
               "{replace,replace_range,insert_after,delete_range} with N:hash anchors",
               parse_bad);
      return safe_strdup(e);
   }

   char *newc = NULL;
   size_t newlen = 0;
   hl_edit_fail_t fail;
   hl_edit_status_t est =
       hashline_edit_apply(content, clen, &view, ops, (size_t)nops, &newc, &newlen, &fail);
   free(ops);
   hashline_snapshot_view_free(&view);

   if (est != HL_EDIT_OK)
   {
      cJSON *p = cJSON_CreateObject();
      const char *status = (est == HL_EDIT_CONFLICT) ? "conflict"
                           : (est == HL_EDIT_BADOP)  ? "bad_op"
                                                     : "stale_anchor";
      cJSON_AddStringToObject(p, "status", status);
      cJSON_AddStringToObject(p, "path", actual_path);
      cJSON_AddStringToObject(p, "reason", fail.reason ? fail.reason : "error");
      if (fail.failed_op >= 0)
         cJSON_AddNumberToObject(p, "op_index", fail.failed_op);
      if (est == HL_EDIT_STALE)
      {
         int wrong_tag = (fail.reason && strcmp(fail.reason, "hash_mismatch") == 0);
         if (wrong_tag)
         {
            /* The file still matches the read snapshot; only the op's anchor hash
             * was wrong (a mis-referenced ordinal). The ORIGINAL snapshot is still
             * valid — echo it and tell the model to fix the anchor, not re-read. */
            cJSON_AddStringToObject(p, "snapshot_id", snapshot_id);
            cJSON_AddStringToObject(
                p, "hint",
                "the anchor hash does not match this line; the file is unchanged — retry against "
                "the same snapshot_id with the correct N:hash anchor (the ordinal is "
                "authoritative; you may omit the hash)");
         }
         else
         {
            /* The file diverged from the snapshot. Mint a fresh snapshot of the
             * current bytes so the model can retry without a blind re-read. */
            char *fresh = hashline_snapshot_mint(sid, actual_path, content, clen);
            if (fresh)
               cJSON_AddStringToObject(p, "snapshot_id", fresh);
            cJSON_AddStringToObject(p, "hint",
                                    "file changed since read; retry edits against the new "
                                    "snapshot_id using these anchors");
            free(fresh);
         }
         int cs = fail.ctx_start > 0 ? fail.ctx_start : 1;
         int ce = fail.ctx_end > 0 ? fail.ctx_end : cs;
         cs = (cs > 3) ? cs - 3 : 1;
         ce = ce + 3;
         cJSON *rows = hl_context_rows(content, clen, cs, ce);
         if (rows)
            cJSON_AddItemToObject(p, "context", rows);
      }
      else
         cJSON_AddStringToObject(
             p, "hint", "ops conflict or reference invalid lines; fix the batch and retry");
      char *out = cJSON_PrintUnformatted(p);
      cJSON_Delete(p);
      free(content);
      free(newc);
      return out ? out : safe_strdup("error: out of memory");
   }

   if (dry_run)
   {
      diff_result_t dr;
      cJSON *p = cJSON_CreateObject();
      cJSON_AddStringToObject(p, "status", "dry_run");
      cJSON_AddStringToObject(p, "path", actual_path);
      if (diff_compute(content, newc, &dr) == 0)
      {
         char *summary = diff_format_summary(&dr);
         char *unified = diff_format_unified(content, newc, &dr);
         cJSON_AddStringToObject(p, "summary", summary ? summary : "no change");
         cJSON_AddItemToObject(p, "diff", diff_result_to_json(&dr));
         if (unified && unified[0])
            cJSON_AddStringToObject(p, "unified_diff", unified);
         free(summary);
         free(unified);
      }
      char blast[2048];
      blast[0] = '\0';
      guardrails_blast_radius_advisory(resolved, blast, sizeof(blast));
      if (blast[0])
         cJSON_AddStringToObject(p, "blast_radius", blast);
      cJSON_AddStringToObject(p, "hint", "dry_run only — no file was written");
      char *out = cJSON_PrintUnformatted(p);
      cJSON_Delete(p);
      free(content);
      free(newc);
      return out ? out : safe_strdup("error: out of memory");
   }

   /* Commit through the gated writer (re-resolves path, enforces write guards,
    * returns the structured diff). Byte-preserving new image built above. */
   char *result = tool_write_file(path, newc);
   free(content);
   free(newc);
   return result;
}

char *append_write_slop_advisory(const char *result, const slop_finding_t *slop, int nslop)
{
   if (!result || !slop || nslop <= 0)
      return result ? safe_strdup(result) : NULL;

   cJSON *json = cJSON_Parse(result);
   if (!json || !cJSON_IsObject(json))
   {
      cJSON_Delete(json);
      size_t rlen = strlen(result);
      char warn_buf[2048];
      int wpos = snprintf(warn_buf, sizeof(warn_buf), "\n\nslop advisory (%d finding(s)):", nslop);
      for (int si = 0; si < nslop && wpos < (int)sizeof(warn_buf) - 80; si++)
         wpos += snprintf(warn_buf + wpos, sizeof(warn_buf) - (size_t)wpos, "\n  line %d [%s] %s",
                          slop[si].line_number, slop_category_label(slop[si].category),
                          slop[si].excerpt);
      size_t wlen = (size_t)wpos;
      char *augmented = malloc(rlen + wlen + 1);
      if (!augmented)
         return safe_strdup(result);
      memcpy(augmented, result, rlen);
      memcpy(augmented + rlen, warn_buf, wlen + 1);
      return augmented;
   }

   cJSON *arr = cJSON_CreateArray();
   if (!arr)
   {
      cJSON_Delete(json);
      return safe_strdup(result);
   }
   for (int si = 0; si < nslop; si++)
   {
      cJSON *item = cJSON_CreateObject();
      if (!item)
         continue;
      cJSON_AddNumberToObject(item, "line", slop[si].line_number);
      cJSON_AddStringToObject(item, "category", slop_category_label(slop[si].category));
      cJSON_AddStringToObject(item, "excerpt", slop[si].excerpt);
      cJSON_AddItemToArray(arr, item);
   }
   cJSON_AddItemToObject(json, "slop_advisory", arr);
   char *augmented = cJSON_PrintUnformatted(json);
   cJSON_Delete(json);
   if (augmented)
      return augmented;
   return safe_strdup(result);
}

char *tool_list_files(const char *path, const char *pattern)
{
   if (pattern && strstr(pattern, ".."))
      return safe_strdup("error: pattern must not contain '..'");
   char cwd_path[MAX_PATH_LEN];
   const char *actual_path = path_in_thread_cwd(path, cwd_path, sizeof(cwd_path));
   char resolved[MAX_PATH_LEN];
   const char *err = guardrails_validate_file_path(actual_path, resolved, sizeof(resolved));
   if (err)
      return safe_strdup(err);

   /* Route the glob through the workspace provider (shared = direct glob).
    * list() reports zero matches as count 0 (the old GLOB_NOMATCH), so the
    * recursive double-star fallback keys off an empty first pass as before. */
   const workspace_provider_t *ws = workspace_provider_active();
   char **entries = NULL;
   int n = 0;
   if (ws->list(ws, actual_path, pattern, &entries, &n) != 0)
      return safe_strdup("error: glob failed");
   if (n == 0 && pattern && strncmp(pattern, "**/", 3) == 0)
   {
      ws_provider_free_list(entries, n);
      entries = NULL;
      if (ws->list(ws, actual_path, pattern + 3, &entries, &n) != 0)
         return safe_strdup("error: glob failed");
   }

   size_t buf_size = agent_tool_output_cap();
   char *buf = malloc(buf_size + 1);
   if (!buf)
   {
      ws_provider_free_list(entries, n);
      return safe_strdup("error: out of memory");
   }
   size_t pos = 0;
   int count = 0;
   for (int i = 0; i < n && count < AGENT_MAX_LIST_FILES; i++)
   {
      size_t plen = strlen(entries[i]);
      if (pos + plen + 1 >= buf_size)
         break;
      memcpy(buf + pos, entries[i], plen);
      pos += plen;
      buf[pos++] = '\n';
      count++;
   }
   buf[pos] = '\0';

   ws_provider_free_list(entries, n);
   return buf;
}

/* Item 5: Verify tool - check assertions */

/* Direct HTTP HEAD status check via sockets + OpenSSL */
static int http_head_status(const char *url)
{
   int use_ssl;
   int port;
   const char *p;

   if (strncmp(url, "https://", 8) == 0)
   {
      use_ssl = 1;
      port = 443;
      p = url + 8;
   }
   else if (strncmp(url, "http://", 7) == 0)
   {
      use_ssl = 0;
      port = 80;
      p = url + 7;
   }
   else
      return -1;

   /* Parse host and path */
   char host[256];
   char path[2048];
   const char *slash = strchr(p, '/');
   const char *colon = strchr(p, ':');
   size_t hostlen;

   if (colon && (!slash || colon < slash))
   {
      hostlen = (size_t)(colon - p);
      port = atoi(colon + 1);
   }
   else
      hostlen = slash ? (size_t)(slash - p) : strlen(p);

   if (hostlen == 0 || hostlen >= sizeof(host))
      return -1;
   memcpy(host, p, hostlen);
   host[hostlen] = '\0';
   snprintf(path, sizeof(path), "%s", slash ? slash : "/");

   /* Connect with 5s timeout */
   char port_str[16];
   snprintf(port_str, sizeof(port_str), "%d", port);

   struct addrinfo hints = {0}, *res = NULL;
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
      return -1;

   int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
   if (fd < 0)
   {
      freeaddrinfo(res);
      return -1;
   }

   int flags = fcntl(fd, F_GETFL, 0);
   fcntl(fd, F_SETFL, flags | O_NONBLOCK);

   int rc = connect(fd, res->ai_addr, res->ai_addrlen);
   freeaddrinfo(res);

   if (rc < 0 && errno != EINPROGRESS)
   {
      close(fd);
      return -1;
   }
   if (rc < 0)
   {
      struct pollfd pfd = {fd, POLLOUT, 0};
      if (poll(&pfd, 1, 5000) <= 0)
      {
         close(fd);
         return -1;
      }
      int err = 0;
      socklen_t errlen = sizeof(err);
      getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
      if (err)
      {
         close(fd);
         return -1;
      }
   }
   fcntl(fd, F_SETFL, flags);

   /* Set 10s overall timeout */
   struct timeval tv = {10, 0};
   setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

   SSL *ssl = NULL;
   if (use_ssl)
   {
      /* Re-use the global SSL_CTX from agent_http_init() via a local context */
      SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
      if (!ctx)
      {
         close(fd);
         return -1;
      }
      SSL_CTX_set_default_verify_paths(ctx);
      SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

      ssl = SSL_new(ctx);
      SSL_CTX_free(ctx); /* SSL holds a ref */
      if (!ssl)
      {
         close(fd);
         return -1;
      }
      SSL_set_fd(ssl, fd);
      SSL_set_tlsext_host_name(ssl, host);
      SSL_set1_host(ssl, host);
      if (SSL_connect(ssl) <= 0)
      {
         SSL_free(ssl);
         close(fd);
         return -1;
      }
   }

   /* Send HEAD request */
   char req[4096];
   int reqlen = snprintf(req, sizeof(req),
                         "HEAD %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);

   if (ssl)
   {
      if (SSL_write(ssl, req, reqlen) <= 0)
      {
         SSL_shutdown(ssl);
         SSL_free(ssl);
         close(fd);
         return -1;
      }
   }
   else
   {
      if (send(fd, req, (size_t)reqlen, 0) <= 0)
      {
         close(fd);
         return -1;
      }
   }

   /* Read response status line */
   char resp[4096];
   int rlen = 0;
   while (rlen < (int)sizeof(resp) - 1)
   {
      int n;
      if (ssl)
         n = SSL_read(ssl, resp + rlen, (int)sizeof(resp) - 1 - rlen);
      else
         n = (int)recv(fd, resp + rlen, sizeof(resp) - 1 - (size_t)rlen, 0);
      if (n <= 0)
         break;
      rlen += n;
      resp[rlen] = '\0';
      if (strstr(resp, "\r\n"))
         break; /* got status line at minimum */
   }

   if (ssl)
   {
      SSL_shutdown(ssl);
      SSL_free(ssl);
   }
   close(fd);

   /* Parse "HTTP/1.x NNN" */
   if (rlen < 12 || (strncmp(resp, "HTTP/1.0", 8) != 0 && strncmp(resp, "HTTP/1.1", 8) != 0))
      return -1;

   int code = atoi(resp + 9);
   return (code >= 100 && code <= 999) ? code : -1;
}

char *tool_verify(const char *check_type, const char *target, const char *expected)
{
   cJSON *result = cJSON_CreateObject();

   if (strcmp(check_type, "http_status") == 0)
   {
      /* Direct HTTP HEAD request (no shell) */
      int code = http_head_status(target);
      if (code < 0)
      {
         cJSON_AddBoolToObject(result, "pass", 0);
         cJSON_AddStringToObject(result, "reason", "HTTP request failed");
      }
      else
      {
         char status[16];
         snprintf(status, sizeof(status), "%d", code);
         int pass = expected ? (strcmp(status, expected) == 0) : (status[0] == '2');
         cJSON_AddBoolToObject(result, "pass", pass);
         cJSON_AddStringToObject(result, "actual", status);
         cJSON_AddStringToObject(result, "expected", expected ? expected : "2xx");
      }
   }
   else if (strcmp(check_type, "file_contains") == 0)
   {
      char resolved[MAX_PATH_LEN];
      const char *verr = guardrails_validate_file_path(target, resolved, sizeof(resolved));
      if (verr)
      {
         cJSON_AddBoolToObject(result, "pass", 0);
         cJSON_AddStringToObject(result, "reason", verr);
         char *json = cJSON_PrintUnformatted(result);
         cJSON_Delete(result);
         return json;
      }
      FILE *f = fopen(target, "r");
      if (!f)
      {
         cJSON_AddBoolToObject(result, "pass", 0);
         cJSON_AddStringToObject(result, "reason", "file not found");
      }
      else
      {
         char buf[AGENT_TOOL_OUTPUT_MAX + 1];
         size_t n = fread(buf, 1, AGENT_TOOL_OUTPUT_MAX, f);
         buf[n] = '\0';
         fclose(f);
         int pass = expected && strstr(buf, expected) != NULL;
         cJSON_AddBoolToObject(result, "pass", pass);
         if (!pass)
            cJSON_AddStringToObject(result, "reason", "string not found in file");
      }
   }
   else if (strcmp(check_type, "command_succeeds") == 0)
   {
      if (agent_tools_parent_write_guard_root())
      {
         cJSON_AddBoolToObject(result, "pass", 0);
         cJSON_AddStringToObject(
             result, "reason",
             "command_succeeds is blocked while the parent worktree is read-only");
      }
      /* Reject commands with shell metacharacters */
      else if (has_shell_metachar(target))
      {
         cJSON_AddBoolToObject(result, "pass", 0);
         cJSON_AddStringToObject(result, "reason", "command contains shell metacharacters");
      }
      else
      {
         /* Parse into argv and exec directly without shell */
         char *tokens[64];
         int tc = shlex_split(target, tokens, 64);
         if (tc <= 0)
         {
            cJSON_AddBoolToObject(result, "pass", 0);
            cJSON_AddStringToObject(result, "reason", "empty command");
         }
         else
         {
            const char *argv[65];
            for (int j = 0; j < tc && j < 64; j++)
               argv[j] = tokens[j];
            argv[tc] = NULL;
            char *output = NULL;
            int rc = safe_exec_capture(argv, &output, agent_tool_output_cap());
            int pass = (rc == 0);
            cJSON_AddBoolToObject(result, "pass", pass);
            cJSON_AddNumberToObject(result, "exit_code", rc);
            free(output);
            for (int j = 0; j < tc; j++)
               free(tokens[j]);
         }
      }
   }
   else
   {
      cJSON_AddBoolToObject(result, "pass", 0);
      cJSON_AddStringToObject(result, "reason", "unknown check_type");
   }

   char *json = cJSON_PrintUnformatted(result);
   cJSON_Delete(result);
   return json;
}

/* --- grep/search: pattern search in files with regex support --- */
char *tool_grep(const char *path, const char *pattern, int max_results)
{
   if (!path || !pattern)
      return safe_strdup("error: missing path or pattern");
   if (max_results <= 0)
      max_results = 50;
   if (max_results > 200)
      max_results = 200;

   char cwd_path[MAX_PATH_LEN];
   const char *actual_path = path_in_thread_cwd(path, cwd_path, sizeof(cwd_path));
   char resolved[MAX_PATH_LEN];
   const char *verr = guardrails_validate_file_path(actual_path, resolved, sizeof(resolved));
   if (verr)
      return safe_strdup(verr);

   struct stat st;
   if (stat(actual_path, &st) != 0)
      return safe_strdup("error: path does not exist");

   char max_str[16];
   snprintf(max_str, sizeof(max_str), "%d", max_results);

   // clang-format off
   const char *argv[] = {"grep", "--binary-files=without-match", "-rn", "--exclude-dir=.git", "--exclude-dir=.aimee", "--exclude-dir=build", "--exclude-dir=dist", "--exclude-dir=node_modules", "-m", max_str, "--", pattern, actual_path, NULL};
   // clang-format on
   char *output = NULL;
   int rc = safe_exec_capture(argv, &output, agent_tool_output_cap());

   if (rc != 0 && rc != 1 && (!output || !output[0]))
   {
      free(output);
      return safe_strdup("no matches found");
   }

   return output ? output : safe_strdup("no matches found");
}
/* --- git_diff: show working tree changes --- */
char *tool_git_diff(const char *repo_path, const char *ref)
{
   if (!repo_path)
      return safe_strdup("error: missing repo path");

   char cwd_path[MAX_PATH_LEN];
   const char *actual_path = path_in_thread_cwd(repo_path, cwd_path, sizeof(cwd_path));
   struct stat st;
   if (stat(actual_path, &st) != 0 || !S_ISDIR(st.st_mode))
      return safe_strdup("error: repo path is not a directory");

   const char *argv[8];
   int ai = 0;
   argv[ai++] = "git";
   argv[ai++] = "-C";
   argv[ai++] = actual_path;
   argv[ai++] = "diff";
   if (ref && ref[0])
      argv[ai++] = ref;
   argv[ai] = NULL;

   const workspace_provider_t *ws = workspace_provider_active();
   char *output = NULL;
   int rc = ws->exec(ws, argv, &output, agent_tool_output_cap());
   if (rc != 0 && (!output || !output[0]))
   {
      free(output);
      return safe_strdup("error: git diff failed");
   }
   return output ? output : safe_strdup("");
}

/* --- git_status: show working tree status --- */

char *tool_git_status(const char *repo_path)
{
   if (!repo_path)
      return safe_strdup("error: missing repo path");

   char cwd_path[MAX_PATH_LEN];
   const char *actual_path = path_in_thread_cwd(repo_path, cwd_path, sizeof(cwd_path));
   struct stat st;
   if (stat(actual_path, &st) != 0 || !S_ISDIR(st.st_mode))
      return safe_strdup("error: repo path is not a directory");

   const char *argv[] = {"git", "-C", actual_path, "status", "--porcelain", NULL};
   const workspace_provider_t *ws = workspace_provider_active();
   char *output = NULL;
   int rc = ws->exec(ws, argv, &output, agent_tool_output_cap());
   if (rc != 0 && (!output || !output[0]))
   {
      free(output);
      return safe_strdup("error: git status failed");
   }
   return output ? output : safe_strdup("");
}

/* --- env_get: query environment variables safely --- */

char *tool_env_get(const char *name)
{
   if (!name || !name[0])
      return safe_strdup("error: missing variable name");

   /* Reject names with shell metacharacters */
   for (const char *p = name; *p; p++)
   {
      if (!isalnum((unsigned char)*p) && *p != '_')
         return safe_strdup("error: invalid variable name");
   }

   const char *val = getenv(name);
   if (!val)
      return safe_strdup("(not set)");
   return safe_strdup(val);
}

/* --- test: check file/dir existence, permissions, types --- */

char *tool_test(const char *path, const char *check)
{
   if (!path)
      return safe_strdup("{\"pass\":false,\"reason\":\"missing path\"}");
   if (!check)
      check = "exists";

   char cwd_path[MAX_PATH_LEN];
   const char *actual_path = path_in_thread_cwd(path, cwd_path, sizeof(cwd_path));
   struct stat st;
   int exists = (lstat(actual_path, &st) == 0);

   cJSON *result = cJSON_CreateObject();

   if (strcmp(check, "exists") == 0)
   {
      cJSON_AddBoolToObject(result, "pass", exists);
      if (exists)
      {
         cJSON_AddStringToObject(result, "type",
                                 S_ISDIR(st.st_mode)   ? "directory"
                                 : S_ISLNK(st.st_mode) ? "symlink"
                                 : S_ISREG(st.st_mode) ? "file"
                                                       : "other");
         cJSON_AddNumberToObject(result, "size", (double)st.st_size);
      }
   }
   else if (strcmp(check, "is_file") == 0)
   {
      cJSON_AddBoolToObject(result, "pass", exists && S_ISREG(st.st_mode));
   }
   else if (strcmp(check, "is_dir") == 0)
   {
      cJSON_AddBoolToObject(result, "pass", exists && S_ISDIR(st.st_mode));
   }
   else if (strcmp(check, "readable") == 0)
   {
      cJSON_AddBoolToObject(result, "pass", access(actual_path, R_OK) == 0);
   }
   else if (strcmp(check, "writable") == 0)
   {
      cJSON_AddBoolToObject(result, "pass", access(actual_path, W_OK) == 0);
   }
   else if (strcmp(check, "executable") == 0)
   {
      cJSON_AddBoolToObject(result, "pass", access(actual_path, X_OK) == 0);
   }
   else
   {
      cJSON_AddBoolToObject(result, "pass", 0);
      cJSON_AddStringToObject(result, "reason", "unknown check type");
   }

   char *json = cJSON_PrintUnformatted(result);
   cJSON_Delete(result);
   return json;
}

/* Item 7: Git log tool (safe: no shell, uses fork/exec) */
char *tool_git_log(const char *repo_path, int count)
{
   if (count <= 0)
      count = 10;
   if (count > 50)
      count = 50;

   char cwd_path[MAX_PATH_LEN];
   const char *actual_path = path_in_thread_cwd(repo_path, cwd_path, sizeof(cwd_path));
   /* Validate repo_path is a directory */
   struct stat st;
   if (stat(actual_path, &st) != 0 || !S_ISDIR(st.st_mode))
      return safe_strdup("error: repo path is not a directory");

   char count_str[16];
   snprintf(count_str, sizeof(count_str), "%d", count);

   const char *argv[] = {"git", "-C", actual_path, "log", "--oneline", "-n", count_str, NULL};
   char *output = NULL;
   int rc = safe_exec_capture(argv, &output, agent_tool_output_cap());

   if (rc != 0 && (!output || !output[0]))
   {
      free(output);
      return safe_strdup("error: git log failed");
   }

   return output ? output : safe_strdup("");
}

/* Map internal tool arg format to guardrail-compatible JSON.
 * Guardrails expect "file_path" for edit tools and "command" for Bash. */
char *delegation_request_input(const char *question) __attribute__((weak));
char *delegation_request_input(const char *question)
{
   (void)question;
   return NULL;
}

char *tool_request_input(const char *question)
{
   if (!question || !question[0])
      return safe_strdup("error: missing question");

   char *reply = delegation_request_input(question);
   if (!reply)
      return safe_strdup("error: request_input is only available during delegated execution");

   return reply;
}

char *tool_code_search(const char *query, const char *project, int max_results)
{
   if (!query || !query[0])
      return safe_strdup("error: missing query");

   if (max_results <= 0)
      max_results = 50;
   if (max_results > 200)
      max_results = 200;

   code_search_hit_t *hits = calloc((size_t)max_results, sizeof(code_search_hit_t));
   if (!hits)
      return safe_strdup("error: out of memory");

   cJSON *arr = cJSON_CreateArray();
   int overlay_count = agent_source_append_overlay_code_hits(arr, query, project, max_results);
   int remaining = max_results - overlay_count;
   int count = remaining > 0 ? kb_client_index_code_search(query, project, hits, remaining) : 0;

   for (int i = 0; i < count; i++)
   {
      cJSON *h = cJSON_CreateObject();
      cJSON_AddStringToObject(h, "project", hits[i].project);
      cJSON_AddStringToObject(h, "file", hits[i].file_path);
      cJSON_AddStringToObject(h, "snippet", hits[i].snippet);
      cJSON_AddNumberToObject(h, "rank", hits[i].rank);
      agent_source_add_index_freshness(h, hits[i].project, hits[i].file_path);
      cJSON_AddItemToArray(arr, h);
   }
   free(hits);

   char *json = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return json ? json : safe_strdup("[]");
}

/* --- Investigation note tools (delegate-facing) ---
 * Shared knowledge lives behind the knowledge service; server-side delegates
 * reach it via the kb_client RPC bridge. */

static char *render_notes_json_to_text(const char *json, const char *empty_msg, int include_content,
                                       const char *prefix)
{
   if (!json)
      return safe_strdup("error: knowledge service unavailable for notes");
   cJSON *resp = cJSON_Parse(json);
   if (!resp)
      return safe_strdup("error: invalid response from knowledge service");
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      char err[256];
      snprintf(err, sizeof(err), "error: %s",
               cJSON_IsString(msg) ? msg->valuestring : "notes lookup failed");
      cJSON_Delete(resp);
      return safe_strdup(err);
   }
   cJSON *notes = cJSON_GetObjectItemCaseSensitive(resp, "notes");
   int count = cJSON_IsArray(notes) ? cJSON_GetArraySize(notes) : 0;
   if (count == 0)
   {
      char *out = safe_strdup(empty_msg);
      cJSON_Delete(resp);
      return out;
   }
   char buf[8192];
   int pos = snprintf(buf, sizeof(buf), prefix, count);
   cJSON *n = NULL;
   cJSON_ArrayForEach(n, notes)
   {
      if (pos >= (int)sizeof(buf) - 1024)
         break;
      cJSON *t = cJSON_GetObjectItemCaseSensitive(n, "title");
      cJSON *tg = cJSON_GetObjectItemCaseSensitive(n, "tags");
      cJSON *u = cJSON_GetObjectItemCaseSensitive(n, "updated_at");
      cJSON *c = cJSON_GetObjectItemCaseSensitive(n, "content");
      const char *title = cJSON_IsString(t) ? t->valuestring : "";
      const char *tags = cJSON_IsString(tg) ? tg->valuestring : "";
      const char *updated = cJSON_IsString(u) ? u->valuestring : "";
      if (include_content)
      {
         const char *content = cJSON_IsString(c) ? c->valuestring : "";
         pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                         "### %s\nTags: %s | Updated: %s\n\n%s\n\n---\n\n", title, tags, updated,
                         content);
      }
      else
         pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "- %s [%s] (updated: %s)\n", title,
                         tags, updated);
   }
   cJSON_Delete(resp);
   return safe_strdup(buf);
}

char *tool_create_note(const char *title, const char *content, const char *tags)
{
   if (!title || !title[0])
      return safe_strdup("error: missing 'title'");
   if (!content || !content[0])
      return safe_strdup("error: missing 'content'");

   char *json = kb_client_note_create_json(title, content, tags, "delegate");
   if (!json)
      return safe_strdup("error: knowledge service unavailable for note create");
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return safe_strdup("error: invalid response from knowledge service");
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      char err[256];
      snprintf(err, sizeof(err), "error: failed to create note: %s",
               cJSON_IsString(msg) ? msg->valuestring : "unknown");
      cJSON_Delete(resp);
      return safe_strdup(err);
   }
   cJSON *note = cJSON_GetObjectItemCaseSensitive(resp, "note");
   cJSON *nt = note ? cJSON_GetObjectItemCaseSensitive(note, "title") : NULL;
   cJSON *slug = note ? cJSON_GetObjectItemCaseSensitive(note, "slug") : NULL;
   cJSON *id = note ? cJSON_GetObjectItemCaseSensitive(note, "id") : NULL;
   char buf[512];
   snprintf(buf, sizeof(buf), "Note saved: %s (slug: %s, id: %lld)",
            cJSON_IsString(nt) ? nt->valuestring : "",
            cJSON_IsString(slug) ? slug->valuestring : "",
            cJSON_IsNumber(id) ? (long long)id->valuedouble : 0LL);
   cJSON_Delete(resp);
   return safe_strdup(buf);
}

char *tool_list_notes(const char *tag, int limit)
{
   if (limit <= 0 || limit > 20)
      limit = 20;
   char *json = kb_client_note_list_json((tag && tag[0]) ? tag : NULL, limit);
   char *out = render_notes_json_to_text(json, "No investigation notes found.", 0,
                                         "Investigation notes (%d):\n\n");
   free(json);
   return out;
}

char *tool_search_notes(const char *query)
{
   if (!query || !query[0])
      return safe_strdup("error: missing 'query'");
   char *json = kb_client_note_search_json(query, 10);
   char none[256];
   snprintf(none, sizeof(none), "No notes matching '%s'.", query);
   char *out = render_notes_json_to_text(json, none, 1, "Found %d note(s):\n\n");
   free(json);
   return out;
}
