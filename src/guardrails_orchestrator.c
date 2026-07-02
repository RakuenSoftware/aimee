/* guardrails_orchestrator.c: the `pre_tool_check` entry point and its
 * orchestrator-self-discipline helpers (write-command classification,
 * bash-guard messaging, per-session counters).
 *
 * Path/tool classification and the `c_source_has_bare_string_newline`
 * scanner live in guardrails.c; TDD enforcement lives in guardrails_tdd.c. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aimee.h"
#include "cJSON.h"
#include "computer_use.h"
#include "guardrails_internal.h"
#include "guardrails_semantic.h"
#include "guardrails_blast_radius.h"
#include "workspace_provider.h" /* skip worktree enforcement for a detached (client) workspace */
#include "headers/config.h"
#include "headers/git_verify.h"
#include "headers/skill.h"
#include "kb_client.h"
#if !defined(AIMEE_DB2_DISABLED)
#include "kb_reasoning.h"
#endif
#include "log.h"
#include "platform_path.h"
#include "slop_detect.h"
#include <ctype.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

int is_edit_tool(const char *tool)
{
   return strcmp(tool, "Edit") == 0 || strcmp(tool, "Write") == 0 || strcmp(tool, "MultiEdit") == 0;
}

int is_shell_tool(const char *tool)
{
   return strcmp(tool, "Bash") == 0 || strcmp(tool, "shell") == 0 ||
          strcmp(tool, "run_shell_command") == 0 || strcmp(tool, "exec_command") == 0 ||
          strcmp(tool, "functions.exec_command") == 0;
}

static int is_path_tool(const char *tool)
{
   return is_edit_tool(tool) || strcmp(tool, "Read") == 0 || strcmp(tool, "Grep") == 0 ||
          strcmp(tool, "Glob") == 0;
}

static int is_script_tool(const char *tool)
{
   return strcmp(tool, "execute_script") == 0;
}

static int guardrails_anti_pattern_check(const char *file_path, const char *command,
                                         anti_pattern_t *out, int max)
{
#if defined(AIMEE_DB2_DISABLED)
   return kb_client_anti_pattern_check(file_path, command, out, max);
#else
   return db2_anti_pattern_check(file_path, command, out, max);
#endif
}

static void guardrails_anti_pattern_bump(int64_t id)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)kb_client_anti_pattern_bump(id);
#else
   (void)db2_anti_pattern_bump(id);
#endif
}

static int is_subagent_tool(const char *tool)
{
   return strcmp(tool, "Subagent") == 0;
}

static int looks_like_subagent_tool_name(const char *tool_name)
{
   if (!tool_name || !tool_name[0])
      return 0;

   return strcasestr(tool_name, "agent") != NULL || strcasestr(tool_name, "delegate") != NULL ||
          strcasestr(tool_name, "trigger") != NULL || strcasestr(tool_name, "spawn") != NULL ||
          strcasestr(tool_name, "remote") != NULL;
}

static int input_looks_like_subagent_request(const cJSON *root)
{
   if (!root || !cJSON_IsObject(root))
      return 0;

   static const char *keys[] = {"subagent_type", "agent_type", "prompt", "message",
                                "description",   "task",       "role",   NULL};
   int hits = 0;
   for (int i = 0; keys[i]; i++)
   {
      cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, keys[i]);
      if (item && (cJSON_IsString(item) || cJSON_IsObject(item) || cJSON_IsArray(item)))
         hits++;
   }
   return hits >= 2;
}

static cJSON *guardrails_command_item(cJSON *root)
{
   cJSON *cmd = cJSON_GetObjectItem(root, "command");
   if (cmd && cJSON_IsString(cmd))
      return cmd;
   cmd = cJSON_GetObjectItem(root, "cmd");
   if (cmd && cJSON_IsString(cmd))
      return cmd;
   cJSON *body = cJSON_GetObjectItem(root, "body");
   if (body && cJSON_IsString(body))
      return body;
   return cmd;
}

const char *guardrails_canonical_tool_name(const char *tool_name)
{
   if (!tool_name)
      return "";

   if (strcmp(tool_name, "bash") == 0 || strcmp(tool_name, "Bash") == 0 ||
       strcmp(tool_name, "shell") == 0 || strcmp(tool_name, "run_shell_command") == 0 ||
       strcmp(tool_name, "exec_command") == 0 || strcmp(tool_name, "functions.exec_command") == 0)
      return "Bash";
   if (strcmp(tool_name, "execute_script") == 0)
      return "Bash";
   if (strcmp(tool_name, "write_file") == 0 || strcmp(tool_name, "Write") == 0)
      return "Write";
   if (strcmp(tool_name, "read_file") == 0 || strcmp(tool_name, "Read") == 0)
      return "Read";
   if (strcmp(tool_name, "list_files") == 0 || strcmp(tool_name, "Glob") == 0)
      return "Glob";
   if (strcmp(tool_name, "grep") == 0 || strcmp(tool_name, "Grep") == 0)
      return "Grep";
   if (strcmp(tool_name, "Agent") == 0 || strcmp(tool_name, "spawn_agent") == 0 ||
       strcmp(tool_name, "RemoteTrigger") == 0 || strcmp(tool_name, "Task") == 0)
      return "Subagent";

   return tool_name;
}

static int already_seen(session_state_t *state, const char *path)
{
   for (int i = 0; i < state->seen_count; i++)
   {
      if (strcmp(state->seen_paths[i], path) == 0)
         return 1;
   }
   return 0;
}

static void mark_seen(session_state_t *state, const char *path)
{
   if (state->seen_count >= MAX_SEEN_PATHS)
      return;
   if (already_seen(state, path))
      return;
   snprintf(state->seen_paths[state->seen_count], MAX_SEEN_LEN, "%s", path);
   state->seen_count++;
   state->dirty = 1;
}

static int check_classification(session_state_t *state, classification_t *cls, const char *mode,
                                char *msg_buf, size_t msg_len)
{
   if (cls->severity >= SEV_RED && strcmp(mode, MODE_DENY) == 0)
   {
      snprintf(msg_buf, msg_len, "BLOCKED: %s (%s)", cls->path, cls->reason);
      return 2;
   }

   if (cls->severity >= SEV_YELLOW && !already_seen(state, cls->path))
   {
      LOG_WARN("guardrails", "%s (%s)", cls->path, cls->reason);
      mark_seen(state, cls->path);
   }

   if (cls->severity >= SEV_RED && !already_seen(state, cls->path))
   {
      audit_log("path_danger", "%s (%s)", cls->path, cls->reason);
      mark_seen(state, cls->path);
   }

   return 0;
}

/* Check if a Bash command contains a git branch-mutating invocation
 * (commit, push, reset, rebase, merge, cherry-pick). Working-tree-only
 * operations (pull, stash, restore, checkout, tag, add, clean) are allowed
 * so agents can still sync and inspect on main. */
static int shell_is_cmd_sep(char c)
{
   return c == '\0' || c == ' ' || c == '\t' || c == ';' || c == '&' || c == '|' || c == '(' ||
          c == '\n';
}

static const char *shell_skip_ws(const char *p)
{
   while (p && (*p == ' ' || *p == '\t'))
      p++;
   return p;
}

static int shell_read_token(const char **pp, char *out, size_t out_len)
{
   const char *p = shell_skip_ws(*pp);
   if (!p || !*p || *p == ';' || *p == '&' || *p == '|' || *p == '\n')
      return 0;

   size_t n = 0;
   while (*p && *p != ' ' && *p != '\t' && *p != ';' && *p != '&' && *p != '|' && *p != '\n')
   {
      if (*p == '\'' || *p == '"')
      {
         char quote = *p++;
         while (*p && *p != quote)
         {
            if (n + 1 < out_len)
               out[n++] = *p;
            p++;
         }
         if (*p == quote)
            p++;
         continue;
      }
      if (*p == '\\' && p[1])
         p++;
      if (n + 1 < out_len)
         out[n++] = *p;
      p++;
   }
   if (out_len > 0)
      out[n] = '\0';
   *pp = p;
   return n > 0;
}

static const char *bash_find_git_subcmd_from(const char *cmd, const char *start, const char *subcmd,
                                             const char **after_subcmd)
{
   if (!cmd)
      return NULL;
   if (!start)
      start = cmd;

   int in_single = 0;
   int in_double = 0;
   int escaped = 0;
   for (const char *p = start; *p; p++)
   {
      if (escaped)
      {
         escaped = 0;
         continue;
      }
      if (!in_single && *p == '\\')
      {
         escaped = 1;
         continue;
      }
      if (!in_double && *p == '\'')
      {
         in_single = !in_single;
         continue;
      }
      if (!in_single && *p == '"')
      {
         in_double = !in_double;
         continue;
      }
      if (in_single || in_double)
         continue;

      if (p != cmd && !shell_is_cmd_sep(p[-1]))
         continue;
      if (strncmp(p, "git", 3) != 0 || !shell_is_cmd_sep(p[3]))
         continue;

      const char *q = p + 3;
      char token[128];
      while (shell_read_token(&q, token, sizeof(token)))
      {
         if (strcmp(token, "-C") == 0)
         {
            if (!shell_read_token(&q, token, sizeof(token)))
               break;
            continue;
         }
         if (strncmp(token, "-C", 2) == 0 && token[2])
            continue;
         if (strcmp(token, subcmd) == 0)
         {
            if (after_subcmd)
               *after_subcmd = q;
            return p;
         }
         if (token[0] == '-')
            continue;
         break;
      }
   }
   return NULL;
}

static const char *bash_find_git_subcmd(const char *cmd, const char *subcmd,
                                        const char **after_subcmd)
{
   return bash_find_git_subcmd_from(cmd, cmd, subcmd, after_subcmd);
}

static int bash_push_after_subcmd_is_remote_delete(const char *after_push)
{
   char token[128];
   while (shell_read_token(&after_push, token, sizeof(token)))
   {
      if (strcmp(token, "--delete") == 0 || strcmp(token, "-d") == 0)
         return 1;
      if (token[0] == ':' && token[1])
         return 1;
   }
   return 0;
}

static int bash_has_git_push_requiring_gate(const char *cmd)
{
   const char *scan = cmd;
   const char *after_push = NULL;
   while ((scan = bash_find_git_subcmd_from(cmd, scan, "push", &after_push)) != NULL)
   {
      if (!bash_push_after_subcmd_is_remote_delete(after_push))
         return 1;
      scan = after_push;
   }
   return 0;
}

static int bash_has_git_write(const char *cmd)
{
   if (!cmd)
      return 0;

   static const char *git_write_subcmds[] = {"commit",      "reset",       "rebase", "merge",
                                             "cherry-pick", "cherry_pick", NULL};

   for (int i = 0; git_write_subcmds[i]; i++)
      if (bash_find_git_subcmd(cmd, git_write_subcmds[i], NULL))
         return 1;
   return bash_has_git_push_requiring_gate(cmd);
}

/* Check if a Bash command contains a "gh pr create" invocation.
 * Searches for the literal substring with a word-boundary check on "gh". */
static const char *bash_find_gh_pr_create_from(const char *cmd, const char *start,
                                               const char **after_create)
{
   if (!cmd)
      return NULL;
   if (!start)
      start = cmd;

   int in_single = 0;
   int in_double = 0;
   int escaped = 0;
   for (const char *p = start; *p; p++)
   {
      if (escaped)
      {
         escaped = 0;
         continue;
      }
      if (!in_single && *p == '\\')
      {
         escaped = 1;
         continue;
      }
      if (!in_double && *p == '\'')
      {
         in_single = !in_single;
         continue;
      }
      if (!in_single && *p == '"')
      {
         in_double = !in_double;
         continue;
      }
      if (in_single || in_double)
         continue;

      if (p != cmd && !shell_is_cmd_sep(p[-1]))
         continue;
      if (strncmp(p, "gh pr create", 12) == 0 && shell_is_cmd_sep(p[12]))
      {
         if (after_create)
            *after_create = p + 12;
         return p;
      }
   }
   return NULL;
}

static int bash_has_gh_pr_create(const char *cmd)
{
   return bash_find_gh_pr_create_from(cmd, cmd, NULL) != NULL;
}

static const char *tool_effective_cwd(const cJSON *root, const char *fallback_cwd)
{
   static const char *keys[] = {"workdir", "cwd", "working_dir", "working_directory", NULL};
   for (int i = 0; keys[i]; i++)
   {
      cJSON *j = cJSON_GetObjectItemCaseSensitive((cJSON *)root, keys[i]);
      if (cJSON_IsString(j) && j->valuestring[0])
         return j->valuestring;
   }
   return fallback_cwd;
}

/* Extract the effective git directory from a bash command string.
 * Handles "cd /path && git ..." and "git -C /path ..." patterns.
 * Returns the target dir in out_dir if found, or copies fallback_cwd.
 * out_dir is always NUL-terminated. */
static void bash_git_target_dir(const char *cmd, const char *fallback_cwd, char *out_dir,
                                size_t out_len)
{
   if (!cmd || !out_dir || out_len == 0)
      return;

   /* Pattern 1: "cd /path && ..." or "cd '/path' && ..." */
   const char *p = cmd;
   while (*p == ' ' || *p == '\t')
      p++;
   if (strncmp(p, "cd ", 3) == 0)
   {
      p += 3;
      while (*p == ' ' || *p == '\t')
         p++;
      char quote = 0;
      if (*p == '\'' || *p == '"')
      {
         quote = *p;
         p++;
      }
      const char *start = p;
      if (quote)
      {
         while (*p && *p != quote)
            p++;
      }
      else
      {
         while (*p && *p != ' ' && *p != '\t' && *p != ';' && *p != '&')
            p++;
      }
      size_t plen = (size_t)(p - start);
      if (plen > 0 && plen < out_len)
      {
         memcpy(out_dir, start, plen);
         out_dir[plen] = '\0';
         return;
      }
   }

   /* Pattern 2: "git -C /path ..." */
   const char *gc = strstr(cmd, "git -C ");
   if (!gc)
      gc = strstr(cmd, "git -C\t");
   if (gc)
   {
      const char *dp = gc + 7;
      while (*dp == ' ' || *dp == '\t')
         dp++;
      char quote = 0;
      if (*dp == '\'' || *dp == '"')
      {
         quote = *dp;
         dp++;
      }
      const char *start = dp;
      if (quote)
      {
         while (*dp && *dp != quote)
            dp++;
      }
      else
      {
         while (*dp && *dp != ' ' && *dp != '\t')
            dp++;
      }
      size_t dlen = (size_t)(dp - start);
      if (dlen > 0 && dlen < out_len)
      {
         memcpy(out_dir, start, dlen);
         out_dir[dlen] = '\0';
         return;
      }
   }

   /* Fallback: use the caller's cwd */
   if (fallback_cwd)
      snprintf(out_dir, out_len, "%s", fallback_cwd);
   else
      out_dir[0] = '\0';
}

static int bash_has_explicit_git_target_dir(const char *cmd)
{
   if (!cmd)
      return 0;
   const char *p = cmd;
   while (*p == ' ' || *p == '\t')
      p++;
   if (strncmp(p, "cd ", 3) == 0)
      return 1;
   return strstr(cmd, "git -C ") != NULL || strstr(cmd, "git -C\t") != NULL;
}

static int git_push_option_takes_value(const char *token)
{
   if (!token || !token[0])
      return 0;
   if (strcmp(token, "--repo") == 0 || strcmp(token, "--receive-pack") == 0 ||
       strcmp(token, "--exec") == 0 || strcmp(token, "--push-option") == 0 ||
       strcmp(token, "-o") == 0)
      return 1;
   return 0;
}

static int normalize_push_branch_ref(const char *ref, int allow_head, char *out, size_t out_len)
{
   if (!ref || !ref[0] || !out || out_len == 0)
      return 0;

   if (ref[0] == '+')
      ref++;
   if (!ref[0] || ref[0] == ':')
      return 0;

   if (strncmp(ref, "refs/heads/", 11) == 0)
      ref += 11;

   if (!allow_head && strcmp(ref, "HEAD") == 0)
      return 0;

   size_t len = strlen(ref);
   if (len == 0 || len >= out_len)
      return 0;
   memcpy(out, ref, len);
   out[len] = '\0';
   return 1;
}

static int normalize_push_refspec_branch(const char *token, char *out, size_t out_len)
{
   if (!token || !token[0] || !out || out_len == 0)
      return 0;

   if (token[0] == '+')
      token++;
   if (!token[0] || token[0] == ':')
      return 0;

   const char *colon = strchr(token, ':');
   if (!colon)
      return normalize_push_branch_ref(token, 0, out, out_len);

   char local[256];
   size_t local_len = (size_t)(colon - token);
   if (local_len > 0 && local_len < sizeof(local))
   {
      memcpy(local, token, local_len);
      local[local_len] = '\0';
      if (normalize_push_branch_ref(local, 0, out, out_len))
         return 1;
   }

   /* A refspec like HEAD:refs/heads/topic does not name the local branch,
    * but Codex-style hooks can arrive without tool_input.workdir. In that
    * case the destination branch is the only signal that points back to the
    * linked worktree the command is actually pushing from. */
   if (local_len == 4 && strncmp(token, "HEAD", 4) == 0)
   {
      const char *remote = colon + 1;
      if (strncmp(remote, "refs/heads/", 11) == 0)
         return normalize_push_branch_ref(remote, 0, out, out_len);
      if (strncmp(remote, "refs/", 5) != 0)
         return normalize_push_branch_ref(remote, 0, out, out_len);
   }

   return 0;
}

static int normalize_pr_head_branch(const char *head, char *out, size_t out_len)
{
   if (!head || !head[0])
      return 0;

   const char *branch = head;
   const char *colon = strchr(head, ':');
   if (colon && colon[1])
      branch = colon + 1;

   return normalize_push_branch_ref(branch, 0, out, out_len);
}

static int bash_git_push_local_ref(const char *cmd, char *out, size_t out_len)
{
   const char *scan = cmd;
   const char *after_push = NULL;

   while ((scan = bash_find_git_subcmd_from(cmd, scan, "push", &after_push)) != NULL)
   {
      if (bash_push_after_subcmd_is_remote_delete(after_push))
      {
         scan = after_push;
         continue;
      }

      const char *p = after_push;
      char token[256];
      int positional = 0;
      while (shell_read_token(&p, token, sizeof(token)))
      {
         if (token[0] == '-')
         {
            if (git_push_option_takes_value(token))
               (void)shell_read_token(&p, token, sizeof(token));
            continue;
         }

         positional++;
         if (positional == 1)
            continue; /* remote */
         if (normalize_push_refspec_branch(token, out, out_len))
            return 1;
         break;
      }

      scan = after_push;
   }

   return 0;
}

static int bash_gh_pr_create_head_ref(const char *cmd, char *out, size_t out_len)
{
   const char *scan = cmd;
   const char *after_create = NULL;

   while ((scan = bash_find_gh_pr_create_from(cmd, scan, &after_create)) != NULL)
   {
      const char *p = after_create;
      char token[256];
      while (shell_read_token(&p, token, sizeof(token)))
      {
         if (strncmp(token, "--head=", 7) == 0)
            return normalize_pr_head_branch(token + 7, out, out_len);
         if (strcmp(token, "--head") == 0)
         {
            if (!shell_read_token(&p, token, sizeof(token)))
               return 0;
            return normalize_pr_head_branch(token, out, out_len);
         }
      }

      scan = after_create;
   }

   return 0;
}

static int bash_git_push_target_dir(const char *cmd, const char *fallback_cwd, char *out_dir,
                                    size_t out_len)
{
   bash_git_target_dir(cmd, fallback_cwd, out_dir, out_len);

   char local_ref[256];
   char branch_dir[MAX_PATH_LEN];
   if (bash_git_push_local_ref(cmd, local_ref, sizeof(local_ref)) &&
       (worktree_find_branch_in_repo(out_dir, local_ref, branch_dir, sizeof(branch_dir)) ||
        worktree_find_branch_registered(local_ref, branch_dir, sizeof(branch_dir))))
   {
      snprintf(out_dir, out_len, "%s", branch_dir);
      return 1;
   }

   if (bash_gh_pr_create_head_ref(cmd, local_ref, sizeof(local_ref)) &&
       (worktree_find_branch_in_repo(out_dir, local_ref, branch_dir, sizeof(branch_dir)) ||
        worktree_find_branch_registered(local_ref, branch_dir, sizeof(branch_dir))))
   {
      snprintf(out_dir, out_len, "%s", branch_dir);
      return 1;
   }

   char top[MAX_PATH_LEN + 64];
   snprintf(top, sizeof(top), "git -C '%s' rev-parse --show-toplevel 2>/dev/null", out_dir);
   int rc;
   char *resolved = run_cmd(top, &rc);
   free(resolved);
   return rc == 0;
}

/* Resolve the commit a push / PR-create verify gate should check: the *committed*
 * tip of the ref being pushed or proposed — NOT the live working tree. A PR ships
 * the branch as it exists on the remote (so prefer origin/<head>); a push ships
 * the local ref. Passing this commit to verify_check makes the gate verify the
 * tree that will actually leave the machine, which is correct even when the head
 * branch is checked out in a *different* (stale or foreign) worktree. Returns 1
 * and fills out with a commit SHA, or 0 when no ref resolves — the caller then
 * falls back to the working-tree hash (prior behavior). */
static int bash_gate_expected_commit(const char *cmd, const char *pdir, int is_pr_create, char *out,
                                     size_t out_len)
{
   char ref[256];
   const char *candidates[2];
   int ncand = 0;
   char origin_ref[300];

   if (is_pr_create)
   {
      if (!bash_gh_pr_create_head_ref(cmd, ref, sizeof(ref)))
         return 0;
      snprintf(origin_ref, sizeof(origin_ref), "origin/%s", ref);
      candidates[ncand++] = origin_ref; /* what gh actually proposes */
      candidates[ncand++] = ref;        /* fallback: local branch tip */
   }
   else
   {
      if (!bash_git_push_local_ref(cmd, ref, sizeof(ref)))
         return 0;
      candidates[ncand++] = ref;
   }

   for (int i = 0; i < ncand; i++)
   {
      char rc_cmd[MAX_PATH_LEN + 384];
      snprintf(rc_cmd, sizeof(rc_cmd),
               "git -C '%s' rev-parse --verify --quiet '%s^{commit}' 2>/dev/null", pdir,
               candidates[i]);
      int rc;
      char *res = run_cmd(rc_cmd, &rc);
      if (rc == 0 && res && res[0])
      {
         char *nl = strchr(res, '\n');
         if (nl)
            *nl = '\0';
         snprintf(out, out_len, "%s", res);
         free(res);
         return 1;
      }
      free(res);
   }
   return 0;
}

/* Return 1 when dir is inside a git worktree checkout (not the primary repo
 * checkout). We compare --git-dir to --git-common-dir: they differ in a
 * linked worktree and match in the main checkout. */
static int git_dir_is_linked_worktree(const char *dir)
{
   if (!dir || !dir[0])
      return 0;

   char cmd[MAX_PATH_LEN + 128];
   snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse --git-dir --git-common-dir 2>/dev/null", dir);

   int rc;
   char *out = run_cmd(cmd, &rc);
   if (rc != 0 || !out || !out[0])
   {
      free(out);
      return 0;
   }

   char *nl = strchr(out, '\n');
   if (!nl)
   {
      free(out);
      return 0;
   }

   *nl = '\0';
   char *git_dir = out;
   char *common_dir = nl + 1;
   size_t len = strlen(common_dir);
   while (len > 0 && (common_dir[len - 1] == '\n' || common_dir[len - 1] == '\r'))
      common_dir[--len] = '\0';

   int is_worktree = (git_dir[0] && common_dir[0] && strcmp(git_dir, common_dir) != 0);
   free(out);
   return is_worktree;
}

static int path_has_boundary_prefix(const char *path, const char *prefix)
{
   if (!path || !prefix || !prefix[0])
      return 0;
   size_t len = strlen(prefix);
   return strncmp(path, prefix, len) == 0 && (path[len] == '/' || path[len] == '\0');
}

static int git_checkout_under_configured_workspace(const char *git_root)
{
   if (!git_root || !git_root[0])
      return 0;

   config_t cfg;
   if (config_load(&cfg) != 0)
      return 0;

   char resolved_root[MAX_PATH_LEN];
   const char *root = realpath(git_root, resolved_root) ? resolved_root : git_root;
   for (int i = 0; i < cfg.workspace_count; i++)
   {
      char resolved_ws[MAX_PATH_LEN];
      const char *ws = cfg.workspaces[i];
      if (!ws[0])
         continue;
      if (realpath(ws, resolved_ws))
         ws = resolved_ws;
      if (path_has_boundary_prefix(root, ws))
         return 1;
   }
   return 0;
}

static int git_checkout_on_non_default_branch(const char *dir)
{
   if (!dir || !dir[0])
      return 0;

   char cmd[MAX_PATH_LEN + 128];
   snprintf(cmd, sizeof(cmd), "git -C '%s' branch --show-current 2>/dev/null", dir);
   int rc;
   char *out = run_cmd(cmd, &rc);
   if (rc != 0 || !out || !out[0])
   {
      free(out);
      return 0;
   }

   char *nl = strchr(out, '\n');
   if (nl)
      *nl = '\0';
   int ok = out[0] && strcmp(out, "main") != 0 && strcmp(out, "master") != 0;
   free(out);
   return ok;
}

static int cwd_is_git_checkout(const char *cwd)
{
   char git_root[MAX_PATH_LEN];
   return cwd && cwd[0] && git_repo_root(cwd, git_root, sizeof(git_root)) == 0 && git_root[0];
}

static int git_checkout_is_external_feature_worktree(const char *cwd)
{
   char git_root[MAX_PATH_LEN];
   if (!cwd || !cwd[0] || git_repo_root(cwd, git_root, sizeof(git_root)) != 0 || !git_root[0])
      return 0;
   if (git_checkout_under_configured_workspace(git_root))
      return 0;
   return git_checkout_on_non_default_branch(git_root);
}

static int session_cwd_is_worktree(const char *cwd)
{
   if (!cwd || !cwd[0])
      return 0;
   if (is_aimee_worktree_path(cwd))
      return 1;
   if (git_dir_is_linked_worktree(cwd))
      return 1;
   return git_checkout_is_external_feature_worktree(cwd);
}

/* 1 iff `cwd` lies inside a registered `detached` workspace. For such a
 * workspace the filesystem authority is the CLIENT (workspace-resource-plane
 * §2-3): the cwd is the client's path, which need not exist on this server, and
 * a write/exec there marshals to the serving client through the workspace
 * provider. The worktree write guard validates SERVER-side fs properties (git
 * checkout / worktree), which are meaningless for a detached root — so it must
 * defer to the client and not block here. Safety for the detached case is the
 * client's (it serves its own tree). */
static int cwd_is_detached_workspace(const char *cwd)
{
   if (!cwd || !cwd[0])
      return 0;
   /* The cwd of a detached turn is the CLIENT's path — POSIX (/...) or Windows
    * (C:\... / C:/...). A Windows thin client serving its tree has a drive-letter
    * cwd, so accept that form too (the exemption is what lets its remote agent
    * write to the served working tree). */
   int win_abs = (((cwd[0] >= 'A' && cwd[0] <= 'Z') || (cwd[0] >= 'a' && cwd[0] <= 'z')) &&
                  cwd[1] == ':' && (cwd[2] == '\\' || cwd[2] == '/'));
   if (cwd[0] != '/' && !win_abs)
      return 0;
   config_t cfg;
   config_load(&cfg);
   for (int i = 0; i < cfg.workspace_count; i++)
   {
      const char *ws = cfg.workspaces[i];
      size_t len = ws ? strlen(ws) : 0;
      if (len == 0 || cfg.workspace_providers[i][0] == '\0')
         continue;
      int inside =
          strncmp(cwd, ws, len) == 0 && (cwd[len] == '/' || cwd[len] == '\\' || cwd[len] == '\0');
      if (inside && strcmp(cfg.workspace_providers[i], "detached") == 0)
         return 1;
   }
   return 0;
}

static int shell_command_targets_worktree(const char *tool_name, cJSON *cmd,
                                          const char *effective_cwd)
{
   if (!is_shell_tool(tool_name) || !cmd || !cJSON_IsString(cmd))
      return 0;
   if (is_aimee_worktree_path(cmd->valuestring))
      return 1;

   char target_dir[MAX_PATH_LEN];
   bash_git_target_dir(cmd->valuestring, effective_cwd, target_dir, sizeof(target_dir));
   return session_cwd_is_worktree(target_dir);
}

static cJSON *tool_path_item(cJSON *root)
{
   cJSON *fp = cJSON_GetObjectItem(root, "file_path");
   if (cJSON_IsString(fp))
      return fp;
   return cJSON_GetObjectItem(root, "path");
}

static int path_tool_targets_worktree(const char *tool_name, cJSON *fp, const char *effective_cwd)
{
   if (!is_path_tool(tool_name))
      return 0;
   if (!fp || !cJSON_IsString(fp))
      return is_aimee_worktree_path(effective_cwd);

   char target[MAX_PATH_LEN];
   normalize_path(fp->valuestring, effective_cwd, target, sizeof(target));
   return is_aimee_worktree_path(target);
}

static const char *state_worktree_for_git_root(const session_state_t *state, const char *git_root)
{
   if (!state || !git_root || !git_root[0])
      return NULL;

   for (int i = 0; i < state->worktree_count; i++)
   {
      if (strcmp(state->worktrees[i].git_root, git_root) == 0 &&
          state->worktrees[i].worktree_path[0])
         return state->worktrees[i].worktree_path;
   }
   return NULL;
}

/* Check whether a simple bash command duplicates an MCP tool.
 * Returns a suggestion string if the command is a plain (non-pipeline,
 * non-compound) equivalent of Read, Grep, or Glob; NULL otherwise.
 *
 * The guard fires only when the command has no shell operators (|, &&, ||, ;)
 * that indicate a pipeline or compound expression — those cases are legitimate
 * uses of the shell even when a simpler MCP form exists. */
static const char *bash_mcp_tool_suggestion(const char *command)
{
   if (!command)
      return NULL;

   /* Skip leading whitespace */
   while (*command && isspace((unsigned char)*command))
      command++;

   /* Only fire for simple, single-command invocations.  Pipelines and compound
    * commands are intentional shell use — don't second-guess them. */
   if (strchr(command, '|') || strstr(command, "&&") || strstr(command, "||") ||
       strchr(command, ';'))
      return NULL;

   /* cat <file>  — flag-less form only; "cat -n file" keeps a purpose */
   if (strncmp(command, "cat ", 4) == 0)
   {
      const char *arg = command + 4;
      while (*arg && isspace((unsigned char)*arg))
         arg++;
      if (*arg && *arg != '-')
         return "Use the Read tool instead of 'cat' — it provides line numbers and "
                "structured output.";
   }

   /* head [-n N] <file> */
   if (strncmp(command, "head ", 5) == 0)
      return "Use the Read tool with a 'limit' parameter instead of 'head'.";

   /* tail [-n N] <file> */
   if (strncmp(command, "tail ", 5) == 0)
      return "Use the Read tool with 'limit' and 'offset' parameters instead of 'tail'.";

   /* grep [options] <pattern> <path> */
   if (strncmp(command, "grep ", 5) == 0)
      return "Use the Grep tool instead of 'grep' for indexed, structured search results.";

   /* find <path> -name <glob> */
   if (strncmp(command, "find ", 5) == 0 && strstr(command, "-name "))
      return "Use the Glob tool instead of 'find -name' for fast file pattern matching.";

   return NULL;
}

static int bash_looks_like_symbol_search(const char *command)
{
   if (!command)
      return 0;
   while (*command && isspace((unsigned char)*command))
      command++;
   if (strchr(command, '|') || strstr(command, "&&") || strstr(command, "||") ||
       strchr(command, ';'))
      return 0;
   return strncmp(command, "rg ", 3) == 0 || strncmp(command, "grep ", 5) == 0;
}

static int skill_dispatch_find_symbols_advisory(session_state_t *state, const char *command,
                                                char *msg_buf, size_t msg_len)
{
   if (!state || !bash_looks_like_symbol_search(command))
      return 0;

   config_t cfg;
   config_load(&cfg);
   if (!cfg.skills_dispatch_advisory)
      return 0;

   if (state->skill_find_symbols_advisory_sent)
      return 0;

   state->skill_find_symbols_advisory_sent = 1;
   state->dirty = 1;
   snprintf(msg_buf, msg_len,
            "ADVISORY: find-symbols may apply. Use `aimee index find <symbol>` or "
            "`aimee skill show find-symbols` before broad grep/rg symbol searches.");
   LOG_INFO("guardrails", "skill dispatch advisory: find-symbols");
   return 1;
}

static int skill_dispatch_trigger_advisory(session_state_t *state, const char *project_root,
                                           const char *skill_name, const char *tool_name,
                                           const char *subject, int *sent_flag, const char *message,
                                           char *msg_buf, size_t msg_len)
{
   if (!state || !skill_name || !tool_name || !sent_flag || !message)
      return 0;

   config_t cfg;
   config_load(&cfg);
   if (!cfg.skills_dispatch_advisory || *sent_flag)
      return 0;

   if (!skill_trigger_matches(project_root, skill_name, tool_name, subject))
      return 0;

   *sent_flag = 1;
   state->dirty = 1;
   snprintf(msg_buf, msg_len, "ADVISORY: %s", message);
   LOG_INFO("guardrails", "skill dispatch advisory: %s", skill_name);
   return 1;
}

static int skill_dispatch_condition_waiting_advisory(session_state_t *state,
                                                     const char *project_root, const char *command,
                                                     char *msg_buf, size_t msg_len)
{
   return skill_dispatch_trigger_advisory(
       state, project_root, "condition-based-waiting", "Bash", command,
       &state->skill_condition_waiting_advisory_sent,
       "condition-based-waiting may apply. Wait on an observable readiness condition "
       "instead of a guessed sleep; use `aimee skill show condition-based-waiting`.",
       msg_buf, msg_len);
}

static int skill_dispatch_tdd_advisory(session_state_t *state, const char *project_root,
                                       const char *path, const char *tool_name, char *msg_buf,
                                       size_t msg_len)
{
   return skill_dispatch_trigger_advisory(
       state, project_root, "test-driven-development", tool_name, path,
       &state->skill_tdd_advisory_sent,
       "test-driven-development may apply. Keep the focused test red/green/refactor loop "
       "active; use `aimee skill show test-driven-development`.",
       msg_buf, msg_len);
}

/* --- Orchestrator self-discipline helpers --- */

/* Threshold: warn after this many total hook invocations without delegation. */
#define ORCH_NUDGE_THRESHOLD 10

/* Returns 1 if the path is an implementation source file that an orchestrator
 * should generally delegate rather than edit directly. */
static int is_orch_impl_file(const char *path)
{
   if (!path || !path[0])
      return 0;
   static const char *impl_exts[] = {".c",     ".h",   ".cc",  ".cpp", ".cxx",  ".hpp",
                                     ".go",    ".py",  ".rb",  ".rs",  ".java", ".kt",
                                     ".swift", ".js",  ".ts",  ".tsx", ".jsx",  ".vue",
                                     ".cs",    ".php", ".lua", ".zig", NULL};
   const char *dot = strrchr(path, '.');
   if (!dot)
      return 0;
   for (int i = 0; impl_exts[i]; i++)
   {
      if (strcasecmp(dot, impl_exts[i]) == 0)
         return 1;
   }
   return 0;
}

/* Returns 1 if the path is exempt from orchestrator delegation reminders.
 * Exempt: .aimee/ paths, docs/proposals/ subtree, root-level config files,
 * CLAUDE.md, AGENTS.md, and any path already inside an aimee worktree. */
static int is_orch_exempt_path(const char *path)
{
   if (!path || !path[0])
      return 1;

   /* Already in an aimee worktree — the delegate is editing it, not the orchestrator */
   if (is_aimee_worktree_path(path))
      return 1;

   /* Strip leading absolute path components down to the first interesting segment */
   const char *p = path;

   /* Find the base-name and a usable relative suffix.
    * Scan for well-known directory names anywhere in the path. */
   if (strstr(path, "/.aimee/") || strstr(path, "/.aimee-"))
      return 1;
   if (strstr(path, "/docs/proposals/"))
      return 1;
   if (strstr(path, "/docs/proposals"))
      return 1;

   const char *fname = basename_of(path);

   /* Exact well-known planning/config filenames */
   if (strcmp(fname, "CLAUDE.md") == 0 || strcmp(fname, "AGENTS.md") == 0 ||
       strcmp(fname, "CLAUDE.local.md") == 0 || strcmp(fname, "README.md") == 0)
      return 1;

   /* Root-level config extensions: .yaml, .yml, .json, .toml, .env* */
   const char *dot = strrchr(fname, '.');
   if (dot)
   {
      if (strcasecmp(dot, ".yaml") == 0 || strcasecmp(dot, ".yml") == 0 ||
          strcasecmp(dot, ".json") == 0 || strcasecmp(dot, ".toml") == 0 ||
          strcasecmp(dot, ".md") == 0)
         return 1;
   }

   /* Makefile, CMakeLists.txt, Dockerfile, shell scripts */
   if (strcmp(fname, "Makefile") == 0 || strcmp(fname, "CMakeLists.txt") == 0 ||
       strcmp(fname, "Dockerfile") == 0 || strcmp(fname, "Rules.mk") == 0)
      return 1;

   /* .sh files are build/infra scripts — orchestrators may reasonably touch them */
   if (dot && strcasecmp(dot, ".sh") == 0)
      return 1;

   (void)p; /* silence unused-variable warning */
   return 0;
}

/* tdd_check_impl_write is declared in guardrails_internal.h (defined in
 * guardrails_tdd.c). */

/* Returns 1 when the tool is a write-intent action eligible for semantic scoring. */
static int is_write_intent(const char *tool_name, cJSON *root)
{
   if (is_edit_tool(tool_name))
      return 1;
   if (is_shell_tool(tool_name))
   {
      cJSON *cmd = guardrails_command_item(root);
      if (cmd && cJSON_IsString(cmd) &&
          (is_write_command(cmd->valuestring) || bash_has_gh_pr_create(cmd->valuestring)))
         return 1;
   }
   return 0;
}

int pre_tool_check_inner(const char *tool_name, const char *input_json, session_state_t *state,
                         const char *guardrail_mode, const char *cwd, char *msg_buf, size_t msg_len)
{
   if (!tool_name || !input_json || !state)
      return 0;

   msg_buf[0] = '\0';
   const char *mode = guardrail_mode ? guardrail_mode : MODE_APPROVE;

   /* Increment hook invocation counter for diagnostics */
   state->hook_call_count++;
   state->dirty = 1;

   cJSON *root = cJSON_Parse(input_json);
   if (!root)
      return 0;

   if (computer_use_is_tool_name(tool_name))
   {
      config_t cfg;
      config_load(&cfg);
      computer_use_policy_t policy;
      computer_use_policy_from_config(&cfg, &policy);
      computer_use_decision_t decision = COMPUTER_USE_DECISION_ALLOW;
      char reason[256] = "";
      if (computer_use_classify(&policy, tool_name, input_json, &decision, reason,
                                sizeof(reason)) &&
          decision != COMPUTER_USE_DECISION_ALLOW)
      {
         snprintf(msg_buf, msg_len, "%s: %s",
                  decision == COMPUTER_USE_DECISION_APPROVE ? "APPROVAL_REQUIRED" : "BLOCKED",
                  reason[0] ? reason : "computer-use action rejected by policy");
         cJSON_Delete(root);
         return 2;
      }
   }

   int script_tool = is_script_tool(tool_name);
   tool_name = guardrails_canonical_tool_name(tool_name);
   if (!is_subagent_tool(tool_name) && looks_like_subagent_tool_name(tool_name) &&
       input_looks_like_subagent_request(root))
      tool_name = "Subagent";

   /* Extract common fields */
   cJSON *fp = tool_path_item(root);
   cJSON *cmd = guardrails_command_item(root);
   const char *effective_cwd = tool_effective_cwd(root, cwd);
   int command_targets_worktree = shell_command_targets_worktree(tool_name, cmd, effective_cwd);
   int target_is_worktree = path_tool_targets_worktree(tool_name, fp, effective_cwd);
   if (is_shell_tool(tool_name) && cmd && cJSON_IsString(cmd) &&
       (bash_has_git_push_requiring_gate(cmd->valuestring) ||
        bash_has_gh_pr_create(cmd->valuestring)))
   {
      char gate_target[MAX_PATH_LEN];
      if (bash_git_push_target_dir(cmd->valuestring, effective_cwd, gate_target,
                                   sizeof(gate_target)) &&
          session_cwd_is_worktree(gate_target))
         command_targets_worktree = 1;
   }

   char norm[MAX_PATH_LEN];

   /* Plan mode check: block write tools */
   if (strcmp(state->session_mode, MODE_PLAN) == 0)
   {
      if (script_tool)
      {
         snprintf(msg_buf, msg_len, "BLOCKED: execute_script not allowed in plan mode");
         cJSON_Delete(root);
         return 2;
      }
      if (is_edit_tool(tool_name))
      {
         snprintf(msg_buf, msg_len, "BLOCKED: %s not allowed in plan mode", tool_name);
         cJSON_Delete(root);
         return 2;
      }
      if (is_shell_tool(tool_name) && cmd && cJSON_IsString(cmd) &&
          is_write_command(cmd->valuestring))
      {
         snprintf(msg_buf, msg_len, "BLOCKED: write command not allowed in plan mode");
         cJSON_Delete(root);
         return 2;
      }
   }

   if ((script_tool || is_write_intent(tool_name, root)) && !cwd_is_git_checkout(effective_cwd) &&
       !session_cwd_is_worktree(effective_cwd) && !command_targets_worktree &&
       !target_is_worktree && !cwd_is_detached_workspace(effective_cwd))
   {
      snprintf(msg_buf, msg_len,
               "BLOCKED: write blocked because this session is not running in a worktree. "
               "Enter the session worktree before writing.");
      cJSON_Delete(root);
      return 2;
   }

   /* Verify gate: block push/PR (enforce:true); --show-toplevel keeps worktrees independent. */
   if (is_shell_tool(tool_name) && cmd && cJSON_IsString(cmd) &&
       (bash_has_git_push_requiring_gate(cmd->valuestring) ||
        bash_has_gh_pr_create(cmd->valuestring)))
   {
      char pdir[MAX_PATH_LEN], tc[MAX_PATH_LEN + 64];
      int wrc;
      int is_push = bash_has_git_push_requiring_gate(cmd->valuestring);
      int is_pr_create = bash_has_gh_pr_create(cmd->valuestring);
      int target_ok = bash_git_push_target_dir(cmd->valuestring, effective_cwd, pdir, sizeof(pdir));
      if ((is_push || is_pr_create) && !target_ok)
      {
         snprintf(msg_buf, msg_len,
                  "BLOCKED: %s blocked - cannot resolve target worktree for this command. "
                  "Run from the repo/worktree or use an aimee-managed worktree.",
                  is_push ? "push" : "pr create");
         cJSON_Delete(root);
         return 2;
      }
      snprintf(tc, sizeof(tc), "git -C '%s' rev-parse --show-toplevel 2>/dev/null", pdir);
      char *wt = run_cmd(tc, &wrc);
      if (wt)
      {
         char *nl = strchr(wt, '\n');
         if (nl)
            *nl = '\0';
      }
      const char *vr = (wrc == 0 && wt && wt[0]) ? wt : NULL;
      char vm[256];
      /* Verify the committed tip being pushed/proposed, not the working tree of
       * whatever checkout happens to hold the head branch (which may be a stale
       * or foreign worktree). Falls back to NULL (working-tree hash) if no ref
       * resolves. */
      char expected[64];
      const char *exp = bash_gate_expected_commit(cmd->valuestring, pdir, is_pr_create, expected,
                                                  sizeof(expected))
                            ? expected
                            : NULL;
      /* verify_gate_blocks honors scope (current project only unless
       * cross-project verify is enabled) and the global verify master switch,
       * and does not auto-configure an out-of-scope/unconfigured repo. */
      if (verify_gate_blocks(vr, exp, vm, sizeof(vm)))
      {
         snprintf(msg_buf, msg_len, "BLOCKED: %s blocked — %s. Run 'aimee git verify' first.",
                  is_push ? "push" : "pr create", vm);
         free(wt);
         cJSON_Delete(root);
         return 2;
      }
      free(wt);
   }

   /* Worktree enforcement: force file/path tools and all shell commands that
    * would run in the source checkout into the aimee-managed worktree for this
    * session. Skipped when the target is already inside an aimee worktree or a
    * linked git worktree.
    *
    * Both file AND shell tools are skipped when a `detached` workspace provider
    * is active: that workspace is filesystem-authority on the CLIENT, so its
    * file tools (read/list/stat) and shell commands (tool_bash -> exec_shell)
    * marshal over the reverse channel and run on the client's tree. Forcing them
    * into a server-side worktree would rewrite paths/cwd to a checkout that does
    * not exist on the client. Command-safety and the push/PR verify gate above
    * run independently of this block, so they still apply. */
   const workspace_provider_t *gr_ws_active = workspace_provider_active();
   int gr_detached_active = gr_ws_active && gr_ws_active->kind == WS_PROVIDER_DETACHED;
   if (!gr_detached_active &&
       (is_path_tool(tool_name) || (is_shell_tool(tool_name) && cmd && cJSON_IsString(cmd))))
   {
      /* Determine target path */
      const char *target = effective_cwd;
      char wnorm[MAX_PATH_LEN];
      if (is_path_tool(tool_name) && fp && cJSON_IsString(fp))
      {
         normalize_path(fp->valuestring, effective_cwd, wnorm, sizeof(wnorm));
         target = wnorm;
      }
      char shell_target[MAX_PATH_LEN] = "";
      int shell_has_explicit_target = 0;
      if (is_shell_tool(tool_name) && cmd && cJSON_IsString(cmd))
      {
         if (!((bash_has_git_push_requiring_gate(cmd->valuestring) ||
                bash_has_gh_pr_create(cmd->valuestring)) &&
               bash_git_push_target_dir(cmd->valuestring, effective_cwd, shell_target,
                                        sizeof(shell_target))))
            bash_git_target_dir(cmd->valuestring, effective_cwd, shell_target,
                                sizeof(shell_target));
         shell_has_explicit_target = bash_has_explicit_git_target_dir(cmd->valuestring);
         if (shell_target[0])
            target = shell_target;
      }

      /* Find git root for the target. Skip if target is empty or not an absolute path. */
      char target_dir[MAX_PATH_LEN];
      snprintf(target_dir, sizeof(target_dir), "%s", target);
      /* If target looks like a file, use its directory */
      char *last_slash = strrchr(target_dir, '/');
      if (last_slash && last_slash != target_dir)
      {
         struct stat st;
         if (stat(target_dir, &st) != 0 || !S_ISDIR(st.st_mode))
            *last_slash = '\0';
      }

      /* For shell commands, also check if the command string itself targets a
       * worktree. This handles "cd /path/.aimee-foo-sid/src && make" where cwd
       * is the real repo but the command operates inside the worktree. */
      int cmd_targets_worktree = command_targets_worktree;
      int target_is_worktree = is_aimee_worktree_path(target) || git_dir_is_linked_worktree(target);
      int cwd_is_aimee_worktree = is_aimee_worktree_path(effective_cwd);
      /* Whether the *current directory* is itself a worktree. This gates shell
       * redirection only: relative shell paths resolve against the cwd, so a
       * shell already inside a worktree is left alone. Path tools (Edit/Write/
       * Read/Grep/Glob) carry an absolute, cwd-independent target, so their
       * redirect must NOT depend on the cwd -- otherwise the same file resolves
       * to the real checkout or the worktree depending on the transient cwd,
       * scattering edits across both trees. */
      int cwd_in_worktree = cwd_is_aimee_worktree || git_dir_is_linked_worktree(effective_cwd);

      if (is_shell_tool(tool_name) && cmd && cJSON_IsString(cmd) && target_is_worktree &&
          !shell_has_explicit_target)
      {
         size_t wt_len = strlen(target);
         if (strncmp(effective_cwd, target, wt_len) != 0 ||
             (effective_cwd[wt_len] != '/' && effective_cwd[wt_len] != '\0'))
         {
            LOG_INFO("guardrails", "worktree rewrite (bash): cd %s && %s", target,
                     cmd->valuestring);
            snprintf(msg_buf, msg_len, "cd %s && %s", target, cmd->valuestring);
            cJSON_Delete(root);
            return 3;
         }
      }

      char git_root_buf[MAX_PATH_LEN];
      if (target_dir[0] == '/' && !target_is_worktree && !cmd_targets_worktree &&
          (is_path_tool(tool_name) || !cwd_in_worktree) &&
          git_repo_root(target_dir, git_root_buf, sizeof(git_root_buf)) == 0)
      {
         const char *sid = session_id();
         char wt_path[MAX_PATH_LEN];
         const char *mapped_wt = state_worktree_for_git_root(state, git_root_buf);
         if (mapped_wt && mapped_wt[0])
            snprintf(wt_path, sizeof(wt_path), "%s", mapped_wt);
         else
            worktree_sibling_path(git_root_buf, sid, NULL, wt_path, sizeof(wt_path));

         /* If target is already inside the worktree path, allow */
         size_t wt_len = strlen(wt_path);
         if (strncmp(target, wt_path, wt_len) != 0 ||
             (target[wt_len] != '/' && target[wt_len] != '\0'))
         {
            /* Target is in the real repo, not the worktree; block and redirect. */
            struct stat wt_st;
            if (stat(wt_path, &wt_st) != 0)
            {
               /* Worktree doesn't exist yet — create it */
               worktree_create_sibling(git_root_buf, sid, NULL);
            }

            /* Register mapping in session state so MCP git tools can find it */
            {
               int found = 0;
               for (int i = 0; i < state->worktree_count; i++)
               {
                  if (strcmp(state->worktrees[i].git_root, git_root_buf) == 0)
                  {
                     found = 1;
                     break;
                  }
               }
               if (!found && state->worktree_count < MAX_WORKTREES)
               {
                  worktree_mapping_t *m = &state->worktrees[state->worktree_count];
                  snprintf(m->git_root, sizeof(m->git_root), "%s", git_root_buf);
                  snprintf(m->worktree_path, sizeof(m->worktree_path), "%s", wt_path);
                  state->worktree_count++;
                  state->dirty = 1;
               }
            }

            /* For path tools, silently rewrite or inject the path in the worktree. */
            if (is_path_tool(tool_name))
            {
               size_t gr_len = strlen(git_root_buf);
               char rewritten[MAX_PATH_LEN];
               if (fp && cJSON_IsString(fp))
                  snprintf(rewritten, sizeof(rewritten), "%s%s", wt_path, target + gr_len);
               else
                  snprintf(rewritten, sizeof(rewritten), "%s", wt_path);
               LOG_INFO("guardrails", "worktree rewrite: %s -> %s", target, rewritten);
               snprintf(msg_buf, msg_len, "%s", rewritten);
               cJSON_Delete(root);
               return 1; /* allow with path rewrite */
            }

            /* For bash write commands, silently rewrite by prepending cd to worktree */
            if (is_shell_tool(tool_name) && cmd && cJSON_IsString(cmd))
            {
               if (shell_has_explicit_target)
               {
                  snprintf(msg_buf, msg_len,
                           "BLOCKED: command targets source checkout %s; session is forced to "
                           "worktree %s",
                           target, wt_path);
                  cJSON_Delete(root);
                  return 2;
               }
               LOG_INFO("guardrails", "worktree rewrite (bash): cd %s && %s", wt_path,
                        cmd->valuestring);
               snprintf(msg_buf, msg_len, "cd %s && %s", wt_path, cmd->valuestring);
               cJSON_Delete(root);
               return 3; /* allow with command rewrite */
            }

            /* Fallback: block if we can't determine how to rewrite */
            LOG_INFO("guardrails", "worktree redirect: %s -> %s", target, wt_path);
            snprintf(msg_buf, msg_len, "BLOCKED: write to real repo path. Use worktree instead: %s",
                     wt_path);
            cJSON_Delete(root);
            return 2;
         }
      }
   }

   /* Hard block for write intents the redirect above cannot handle (e.g.
    * script tools) when the session is not running in any worktree. A detached
    * workspace is exempt: its tree lives on the serving client, not here. */
   if ((script_tool || is_write_intent(tool_name, root)) &&
       !session_cwd_is_worktree(effective_cwd) && !command_targets_worktree &&
       !target_is_worktree && !cwd_is_detached_workspace(effective_cwd))
   {
      snprintf(msg_buf, msg_len,
               "BLOCKED: write blocked because this session is not running in a worktree. "
               "Enter the session worktree before writing.");
      cJSON_Delete(root);
      return 2;
   }

   /* Main branch protection: block all git write commands via Bash on main/master.
    * Extract the target directory from the command (cd /path && git ..., or
    * git -C /path ...) so we check the correct repo's branch, not the hook's cwd. */
   if (is_shell_tool(tool_name) && cmd && cJSON_IsString(cmd) &&
       bash_has_git_write(cmd->valuestring))
   {
      char git_dir[MAX_PATH_LEN];
      int target_ok =
          bash_git_push_target_dir(cmd->valuestring, effective_cwd, git_dir, sizeof(git_dir));
      if (bash_has_git_push_requiring_gate(cmd->valuestring) && !target_ok)
      {
         snprintf(msg_buf, msg_len,
                  "BLOCKED: push blocked - cannot resolve pushed worktree for this command. "
                  "Run from the repo/worktree or use an aimee-managed worktree.");
         cJSON_Delete(root);
         return 2;
      }

      char rev_cmd[MAX_PATH_LEN + 128];
      if (git_dir[0])
         snprintf(rev_cmd, sizeof(rev_cmd), "git -C '%s' rev-parse --abbrev-ref HEAD 2>/dev/null",
                  git_dir);
      else
         snprintf(rev_cmd, sizeof(rev_cmd), "git rev-parse --abbrev-ref HEAD 2>/dev/null");

      int brc;
      char *bbranch = run_cmd(rev_cmd, &brc);
      if (brc == 0 && bbranch)
      {
         char *bnl = strchr(bbranch, '\n');
         if (bnl)
            *bnl = '\0';
         if (strcmp(bbranch, "main") == 0 || strcmp(bbranch, "master") == 0)
         {
            free(bbranch);
            snprintf(msg_buf, msg_len,
                     "BLOCKED: git write command on main branch is not allowed. "
                     "Create a feature branch first.");
            cJSON_Delete(root);
            return 2;
         }
      }
      free(bbranch);
   }

   /* Merged-PR enforcement for Bash git push commands.
    * Use the same target directory extraction as main-branch protection above. */
   if (is_shell_tool(tool_name) && cmd && cJSON_IsString(cmd) &&
       bash_has_git_push_requiring_gate(cmd->valuestring))
   {
      char push_git_dir[MAX_PATH_LEN];
      int target_ok = bash_git_push_target_dir(cmd->valuestring, effective_cwd, push_git_dir,
                                               sizeof(push_git_dir));
      if (!target_ok)
      {
         snprintf(msg_buf, msg_len,
                  "BLOCKED: push blocked - cannot resolve pushed worktree for this command. "
                  "Run from the repo/worktree or use an aimee-managed worktree.");
         cJSON_Delete(root);
         return 2;
      }
      if (check_merged_pr_for_branch(push_git_dir))
      {
         snprintf(msg_buf, msg_len,
                  "BLOCKED: branch has a merged PR. Do not push to merged branches. "
                  "Create a new branch for new work.");
         cJSON_Delete(root);
         return 2;
      }
   }

   /* Provider-native sub-agent tools must not escape the active session's
    * guardrails. Delegation should go through aimee's delegate subsystem,
    * which re-enters the same policy path with inherited session state. */
   if (is_subagent_tool(tool_name))
   {
      snprintf(msg_buf, msg_len,
               "BLOCKED: sub-agent tools (Task, Agent, spawn_agent, …) are outside aimee's "
               "guardrail model. Delegate instead: `aimee delegate <role> \"<task>\" "
               "--persona <persona>`, or `aimee delegate roundtable \"<task>\" --mode review` "
               "for a multi-model panel. aimee delegates run on the cheapest capable model, are "
               "cost-tracked, see the shared memory + KB, and inherit this session's guardrails.");
      audit_log("subagent_blocked",
                "provider sub-agent tool blocked — keep delegation inside aimee");
      cJSON_Delete(root);
      return 2;
   }

   /* Anti-pattern check with per-session hit tracking.
    * First/second match: warning surfaced to the agent and stderr.
    * Third+ match of the same pattern: block the tool call.
    * Set AIMEE_ANTIPATTERNS_BYPASS=1 to skip the check entirely (escape hatch
    * for sessions where a stored pattern is misfiring). */
   if (!getenv("AIMEE_ANTIPATTERNS_BYPASS"))
   {
      anti_pattern_t matches[4];
      const char *file_str = (fp && cJSON_IsString(fp)) ? fp->valuestring : NULL;
      const char *cmd_str = (cmd && cJSON_IsString(cmd)) ? cmd->valuestring : NULL;
      int ap_count = guardrails_anti_pattern_check(file_str, cmd_str, matches, 4);
      for (int i = 0; i < ap_count; i++)
      {
         /* Find or create hit counter for this pattern in the session. */
         int slot = -1;
         for (int j = 0; j < state->ap_hit_count; j++)
         {
            if (state->ap_hits[j].pattern_id == matches[i].id)
            {
               slot = j;
               break;
            }
         }
         if (slot < 0 && state->ap_hit_count < MAX_AP_SESSION_HITS)
         {
            slot = state->ap_hit_count++;
            state->ap_hits[slot].pattern_id = matches[i].id;
            state->ap_hits[slot].hits = 0;
         }
         if (slot >= 0)
         {
            state->ap_hits[slot].hits++;
            state->dirty = 1;
         }

         int hits = (slot >= 0) ? state->ap_hits[slot].hits : 1;

         /* Lifetime hit_count should only reflect hits that actually produced
          * a warning or block — bump it here, not inside anti_pattern_check. */
         guardrails_anti_pattern_bump(matches[i].id);

         /* Prefer description; fall back to the pattern string so the message
          * is never empty after the colon. */
         const char *label =
             matches[i].description[0] ? matches[i].description : matches[i].pattern;

         if (hits >= AP_HIT_BLOCK_THRESHOLD)
         {
            snprintf(msg_buf, msg_len,
                     "BLOCKED: anti-pattern triggered %d times this session: %s "
                     "(id=%lld). Start a new session or set AIMEE_ANTIPATTERNS_BYPASS=1 "
                     "to override.",
                     hits, label, (long long)matches[i].id);
            audit_log("antipattern_blocked", "(%dx, id=%lld): %s", hits, (long long)matches[i].id,
                      label);
            cJSON_Delete(root);
            return 2;
         }
         else
         {
            snprintf(msg_buf, msg_len, "WARNING: anti-pattern match (%d/%d): %s (id=%lld)", hits,
                     AP_HIT_BLOCK_THRESHOLD, label, (long long)matches[i].id);
            LOG_WARN("guardrails", "anti-pattern warning (%d/%d, id=%lld): %s", hits,
                     AP_HIT_BLOCK_THRESHOLD, (long long)matches[i].id, label);
         }
      }

      /* Shadow precedent lookup via graph reasoning layer (Phase 1 — log only).
       * Runs when no direct anti-pattern hit was found to check for related cases.
       * Only active when DB2 is available (KB binary context). */
#if !defined(AIMEE_DB2_DISABLED)
      if (ap_count == 0)
      {
         config_t rcfg;
         config_load(&rcfg);
         if (rcfg.reasoning_datalog_command[0] && file_str)
         {
            char path64[65];
            snprintf(path64, sizeof(path64), "%.64s", file_str);
            kb_reasoning_result_t res;
            if (kb_reasoning_query(&rcfg, "citation_reachable(?a, ?b)", NULL, "guardrail", path64,
                                   &res) == 0 &&
                res.n_rows > 0)
               LOG_DEBUG("reasoning.guardrail",
                         "shadow precedent found for '%s' via graph reasoning", path64);
            kb_reasoning_result_free(&res);
         }
      }
#endif
   }

   /* Drift check (warn only) */
   if (state->active_task_id > 0)
   {
      drift_result_t drift;
      const char *file_str = (fp && cJSON_IsString(fp)) ? fp->valuestring : NULL;
      const char *cmd_str = (cmd && cJSON_IsString(cmd)) ? cmd->valuestring : NULL;
      if (kb_client_memory_check_drift(state->active_task_id, file_str, cmd_str, &drift) == 0 &&
          drift.drifted)
      {
         LOG_WARN("guardrails", "drift warning: %s", drift.message);
      }
   }

   /* Edit/Write/MultiEdit: classify file path */
   if (is_edit_tool(tool_name) && fp && cJSON_IsString(fp))
   {
      normalize_path(fp->valuestring, effective_cwd, norm, sizeof(norm));
      classification_t cls = classify_path(norm);
      int rc = check_classification(state, &cls, mode, msg_buf, msg_len);
      if (rc != 0)
      {
         cJSON_Delete(root);
         return rc;
      }
   }

   /* Write on .c/.h files: block bare newlines inside C string literals.
    * Delegates occasionally write printf("ok\n") with a real newline instead of
    * the escape sequence, producing C that fails to compile. Blocking the Write
    * lets the agent self-correct before the bad content lands on disk. */
   if (strcmp(tool_name, "Write") == 0 && fp && cJSON_IsString(fp))
   {
      const char *fpath = fp->valuestring;
      size_t flen = fpath ? strlen(fpath) : 0;
      int is_c_source = (flen >= 2 && (strcmp(fpath + flen - 2, ".c") == 0 ||
                                       strcmp(fpath + flen - 2, ".h") == 0));
      if (is_c_source)
      {
         cJSON *content_item = cJSON_GetObjectItem(root, "content");
         if (content_item && cJSON_IsString(content_item) && content_item->valuestring)
         {
            if (c_source_has_bare_string_newline(content_item->valuestring))
            {
               snprintf(msg_buf, msg_len,
                        "BLOCKED: %s contains a bare (unescaped) newline inside a C string "
                        "literal. String literals cannot span lines — use \\n instead of a "
                        "literal newline character inside quotes.",
                        fpath);
               cJSON_Delete(root);
               return 2;
            }
         }
      }
   }

   /* Read-before-write guard: block Write to an existing file that has not been
    * read in this session. Agents must read a file before overwriting it so they
    * work from current content, not a stale or imagined version. New files (those
    * that do not yet exist on disk) are always allowed through. */
   if (strcmp(tool_name, "Write") == 0 && state && fp && cJSON_IsString(fp))
   {
      normalize_path(fp->valuestring, effective_cwd, norm, sizeof(norm));
      struct stat st;
      if (stat(norm, &st) == 0 && S_ISREG(st.st_mode))
      {
         if (!session_path_was_read(state, norm))
         {
            snprintf(msg_buf, msg_len,
                     "BLOCKED: cannot overwrite existing file '%s' without reading it first. "
                     "Use the Read tool to load the current content, then apply targeted edits "
                     "with Edit, or rewrite with Write after reviewing the file.",
                     norm);
            audit_log("read_before_write", "blocked blind overwrite of %s", norm);
            cJSON_Delete(root);
            return 2;
         }
      }
   }

   /* Truncating-rewrite guard: block a Write that would replace a substantial
    * existing file with a tiny fraction of its content. This is almost always
    * an accident — e.g. a delegate doing a full-file rewrite that is cut off
    * mid-stream (turn/length exhaustion) leaves the file truncated, and that
    * collapsed file then propagates. Targeted Edit/MultiEdit cannot truncate a
    * file this way, so require them for large shrinks. */
   if (strcmp(tool_name, "Write") == 0 && fp && cJSON_IsString(fp))
   {
      normalize_path(fp->valuestring, effective_cwd, norm, sizeof(norm));
      struct stat st;
      cJSON *content_item = cJSON_GetObjectItem(root, "content");
      if (stat(norm, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 4096 && content_item &&
          cJSON_IsString(content_item) && content_item->valuestring)
      {
         size_t new_len = strlen(content_item->valuestring);
         if (new_len < (size_t)(st.st_size / 4))
         {
            snprintf(msg_buf, msg_len,
                     "BLOCKED: Write would shrink '%s' from %lld bytes to %zu (more than 75%% "
                     "smaller), which usually means a full-file rewrite was truncated. Use "
                     "targeted Edit/MultiEdit to change a large existing file; if you truly intend "
                     "to remove nearly all of it, delete the obsolete sections with Edit first.",
                     norm, (long long)st.st_size, new_len);
            audit_log("truncating_write", "blocked %zu-byte overwrite of %lld-byte file %s",
                      new_len, (long long)st.st_size, norm);
            cJSON_Delete(root);
            return 2;
         }
      }
   }

   /* Stale-edit guard: if the file has changed on disk since the last read,
    * only block when the agent's targeted region (old_string) no longer
    * appears verbatim. When the anchor survives the on-disk change, the
    * edit is safe — allowing it avoids spurious blocks for concurrent
    * changes in unrelated regions of the same file. */
   if (strcmp(tool_name, "Edit") == 0 && state && fp && cJSON_IsString(fp))
   {
      normalize_path(fp->valuestring, effective_cwd, norm, sizeof(norm));
      uint64_t stored_hash = session_file_hash(state, norm);
      if (stored_hash != 0 && file_content_hash(norm) != stored_hash)
      {
         cJSON *old_s = cJSON_GetObjectItemCaseSensitive(root, "old_string");
         int region_intact = 0;
         if (old_s && cJSON_IsString(old_s) && old_s->valuestring && old_s->valuestring[0])
            region_intact = file_contains_substring(norm, old_s->valuestring);

         if (!region_intact)
         {
            snprintf(msg_buf, msg_len,
                     "BLOCKED: '%s' has changed on disk since it was last read "
                     "and the target region is no longer present. Re-read the "
                     "file with the Read tool to get the current content before "
                     "making further edits.",
                     norm);
            audit_log("stale_edit", "blocked edit of changed file %s", norm);
            cJSON_Delete(root);
            return 2;
         }
      }
   }

   /* Bash: extract and classify paths from command */
   if (is_shell_tool(tool_name) && cmd && cJSON_IsString(cmd))
   {
      char *paths[32];
      int pcount = extract_paths_shlex(cmd->valuestring, paths, 32);
      for (int i = 0; i < pcount; i++)
      {
         normalize_path(paths[i], effective_cwd, norm, sizeof(norm));
         classification_t cls = classify_path(norm);
         int rc = check_classification(state, &cls, mode, msg_buf, msg_len);
         free(paths[i]);
         if (rc != 0)
         {
            for (int j = i + 1; j < pcount; j++)
               free(paths[j]);
            cJSON_Delete(root);
            return rc;
         }
      }
   }

   /* Skill dispatch advisory: non-blocking reminder for capability skills.
    * Only set when no higher-priority message has already populated msg_buf. */
   if (is_shell_tool(tool_name) && cmd && cJSON_IsString(cmd) && msg_buf[0] == '\0')
   {
      skill_dispatch_find_symbols_advisory(state, cmd->valuestring, msg_buf, msg_len);
   }

   if (is_shell_tool(tool_name) && cmd && cJSON_IsString(cmd) && msg_buf[0] == '\0')
   {
      skill_dispatch_condition_waiting_advisory(state, effective_cwd, cmd->valuestring, msg_buf,
                                                msg_len);
   }

   /* Bash command guard: advisory warning when command duplicates an MCP tool.
    * Only set when no higher-priority message has already populated msg_buf. */
   if (is_shell_tool(tool_name) && cmd && cJSON_IsString(cmd) && msg_buf[0] == '\0')
   {
      const char *suggestion = bash_mcp_tool_suggestion(cmd->valuestring);
      if (suggestion)
      {
         snprintf(msg_buf, msg_len, "ADVISORY: %s", suggestion);
         LOG_INFO("guardrails", "bash command guard: %s", suggestion);
      }
   }

   /* TDD enforcement: runs before orchestrator discipline so enforce mode blocks
    * unconditionally. Warn mode populates msg_buf; enforce mode returns 2. */
   if (is_edit_tool(tool_name) && fp && cJSON_IsString(fp))
   {
      if (msg_buf[0] == '\0')
         skill_dispatch_tdd_advisory(state, effective_cwd, fp->valuestring, tool_name, msg_buf,
                                     msg_len);
      int tdd_rc = tdd_check_impl_write(state, fp->valuestring, msg_buf, msg_len);
      if (tdd_rc == 2)
      {
         cJSON_Delete(root);
         return 2;
      }
   }

   /* Orchestrator self-discipline: advisory reminders for direct source-file edits.
    * Skipped for delegate sessions (is_delegate == 1) — delegates are supposed to
    * edit implementation files; the orchestrator should coordinate instead. */
   if (!state->is_delegate && msg_buf[0] == '\0')
   {
      if (is_edit_tool(tool_name) && fp && cJSON_IsString(fp))
      {
         const char *fpath = fp->valuestring;
         if (is_orch_impl_file(fpath) && !is_orch_exempt_path(fpath))
         {
            state->orch_direct_edits++;
            state->dirty = 1;
            snprintf(msg_buf, msg_len,
                     "ADVISORY: orchestrator is editing an implementation file directly. "
                     "Consider delegating: aimee delegate implement \"%s\"",
                     basename_of(fpath));
            LOG_INFO("guardrails", "orchestrator discipline: direct edit of %s", fpath);
         }
      }
   }

   /* One-time delegation nudge after many direct tool calls without delegation.
    * Only sent once per session; only for orchestrator sessions. */
   if (!state->is_delegate && !state->orch_nudge_sent && msg_buf[0] == '\0' &&
       state->hook_call_count >= ORCH_NUDGE_THRESHOLD)
   {
      state->orch_nudge_sent = 1;
      state->dirty = 1;
      snprintf(msg_buf, msg_len,
               "ADVISORY: %d tool calls without delegation. "
               "Consider: aimee delegate <role> \"<task>\" for implementation work.",
               state->hook_call_count);
      LOG_INFO("guardrails", "orchestrator discipline: delegation nudge after %d calls",
               state->hook_call_count);
   }
   /* Semantic scoring (Phase 0/1): runs after all deterministic checks.
    * Default: enabled=false, dry_run=true — no behavioral change until opted in.
    * When dry_run=true, scores are logged and stored but never affect msg_buf or rc. */
   {
      config_t scfg;
      config_load(&scfg);
      gsem_apply_strictness_arm(&scfg);
      if (scfg.guardrails_semantic_enabled && scfg.guardrails_semantic_command[0] &&
          is_write_intent(tool_name, root))
      {
         gsem_input_t sin;
         gsem_output_t sout;
         gsem_build_input(tool_name, root, effective_cwd, mode, &sin);
         gsem_assess(&sin, scfg.guardrails_semantic_command, &sout);
         const char *action = gsem_policy(&sout, scfg.guardrails_semantic_warn_threshold,
                                          scfg.guardrails_semantic_prompt_threshold,
                                          scfg.guardrails_semantic_block_threshold);
         const char *final_action = action;
         if (!scfg.guardrails_semantic_dry_run && strcmp(action, "block") == 0 &&
             (scfg.guardrails_semantic_advisory_only ||
              !scfg.guardrails_semantic_allow_ml_only_block))
            final_action = "prompt";
         const char *logged_action = scfg.guardrails_semantic_dry_run ? "dry_run" : final_action;
         gsem_record(session_id(), &sin, &sout, logged_action, scfg.guardrails_semantic_dry_run);
         LOG_DEBUG("guardrails.semantic",
                   "tool=%s overall=%.2f rec=%s final=%s dry_run=%d advisory_only=%d", tool_name,
                   sout.overall, sout.recommendation, logged_action,
                   scfg.guardrails_semantic_dry_run, scfg.guardrails_semantic_advisory_only);
         if (!scfg.guardrails_semantic_dry_run && msg_buf[0] == '\0')
         {
            if (strcmp(final_action, "warn") == 0 || strcmp(final_action, "prompt") == 0)
            {
               gsem_format_advisory_message(msg_buf, msg_len, final_action, &sout);
               if (msg_buf[0] != '\0')
                  LOG_INFO("guardrails.semantic", "advisory tool=%s score=%.2f band=%s labels=%s",
                           tool_name, sout.overall, action, sout.labels);
            }
            else if (strcmp(final_action, "block") == 0)
            {
               snprintf(msg_buf, msg_len, "BLOCKED (semantic): risk=%.2f labels=[%s] %s",
                        sout.overall, sout.labels, sout.explanation);
               cJSON_Delete(root);
               return 2;
            }
         }
      }
   }
   if (is_edit_tool(tool_name) && fp && cJSON_IsString(fp)) /* §7 advisory: LAST, fail-open */
      guardrails_blast_radius_advisory(
          normalize_path(fp->valuestring, effective_cwd, norm, sizeof(norm)), msg_buf, msg_len);
   cJSON_Delete(root);
   return 0;
}
