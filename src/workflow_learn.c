/* workflow_learn.c: parse Bash commands for learnable workflow signals and
 * upsert workflow memories through memory_upsert_workflow().
 *
 * Signals are tiny, hand-written heuristics — we want to recognize a
 * handful of common project conventions (PR target branch, test command,
 * active branch) without a real command-line parser. Ambiguous cases are
 * dropped so we do not fabricate rules from noise. */
#include "aimee.h"
#include "cJSON.h"
#include "kb_client.h"
#include "learning_implicit.h"
#include "log.h"
#include <ctype.h>
#include <string.h>
#include <unistd.h>

/* Skip leading ASCII whitespace. */
static const char *wl_skip_ws(const char *s)
{
   while (s && *s && (*s == ' ' || *s == '\t'))
      s++;
   return s;
}

/* Does the command contain `token` as a whitespace-separated word? */
static int wl_cmd_has_word(const char *cmd, const char *token)
{
   if (!cmd || !token)
      return 0;
   size_t tlen = strlen(token);
   for (const char *p = cmd; (p = strstr(p, token)) != NULL; p++)
   {
      int left_ok = (p == cmd) || p[-1] == ' ' || p[-1] == '\t' || p[-1] == ';' || p[-1] == '&' ||
                    p[-1] == '|' || p[-1] == '\n';
      char r = p[tlen];
      int right_ok =
          r == '\0' || r == ' ' || r == '\t' || r == ';' || r == '&' || r == '|' || r == '\n';
      if (left_ok && right_ok)
         return 1;
   }
   return 0;
}

/* Extract the word following a flag like "--base" from a command string.
 * Returns 1 on success with the word copied into out (NUL-terminated,
 * bounded by outlen). Returns 0 if the flag is absent or has no value. */
static int wl_extract_flag_value(const char *cmd, const char *flag, char *out, size_t outlen)
{
   if (!cmd || !flag || !out || outlen == 0)
      return 0;
   const char *p = cmd;
   size_t flen = strlen(flag);
   while ((p = strstr(p, flag)) != NULL)
   {
      if (p != cmd && p[-1] != ' ' && p[-1] != '\t' && p[-1] != '=')
      {
         p += flen;
         continue;
      }
      const char *after = p + flen;
      if (*after == '=')
         after++;
      else if (*after == ' ' || *after == '\t')
         after = wl_skip_ws(after);
      else
      {
         /* "--baseline" should not match "--base". */
         p += flen;
         continue;
      }
      char quote = 0;
      if (*after == '\'' || *after == '"')
      {
         quote = *after;
         after++;
      }
      size_t i = 0;
      while (*after && i + 1 < outlen)
      {
         if (quote && *after == quote)
            break;
         if (!quote && (*after == ' ' || *after == '\t' || *after == ';' || *after == '&' ||
                        *after == '|' || *after == '\n'))
            break;
         out[i++] = *after++;
      }
      out[i] = '\0';
      return i > 0 ? 1 : 0;
   }
   return 0;
}

/* Is "gh pr create" present as a word-boundary match? */
static int wl_has_gh_pr_create(const char *cmd)
{
   if (!cmd)
      return 0;
   for (const char *p = cmd; (p = strstr(p, "gh pr create")) != NULL; p++)
   {
      if (p != cmd && p[-1] != ' ' && p[-1] != '\t' && p[-1] != ';' && p[-1] != '&' &&
          p[-1] != '|' && p[-1] != '(' && p[-1] != '\n')
         continue;
      char end = p[12];
      if (end == '\0' || end == ' ' || end == '\t' || end == ';')
         return 1;
   }
   return 0;
}

int workflow_parse_bash_signal(const char *command, char *signal_type_out, size_t stsize,
                               char *rule_out, size_t rsize)
{
   if (!command || !command[0] || !signal_type_out || stsize == 0 || !rule_out || rsize == 0)
      return 0;

   /* 1. `gh pr create --base X` — PR target branch convention. */
   if (wl_has_gh_pr_create(command))
   {
      char base[128] = "";
      if (wl_extract_flag_value(command, "--base", base, sizeof(base)) && base[0])
      {
         snprintf(signal_type_out, stsize, "pr-target");
         snprintf(rule_out, rsize,
                  "PRs target the `%s` branch (learned from `gh pr create --base %s`).", base,
                  base);
         return 1;
      }
   }

   /* 2. `git push origin <branch>` — active development branch. Skip pushes
    * to the common default branches since those are not project-specific. */
   {
      const char *gp = strstr(command, "git push");
      if (gp)
      {
         const char *after = wl_skip_ws(gp + 8);
         while (*after == '-')
         {
            while (*after && *after != ' ' && *after != '\t')
               after++;
            after = wl_skip_ws(after);
         }
         if (*after)
         {
            while (*after && *after != ' ' && *after != '\t')
               after++;
            after = wl_skip_ws(after);
         }
         char branch[128] = "";
         size_t i = 0;
         while (*after && i + 1 < sizeof(branch) && *after != ' ' && *after != '\t' &&
                *after != ';' && *after != '&' && *after != '|' && *after != '\n')
            branch[i++] = *after++;
         branch[i] = '\0';
         if (branch[0] && strcmp(branch, "main") != 0 && strcmp(branch, "master") != 0 &&
             strcmp(branch, "HEAD") != 0 && branch[0] != '-')
         {
            snprintf(signal_type_out, stsize, "active-branch");
            snprintf(rule_out, rsize,
                     "Active development on branch `%s` (observed via `git push origin %s`).",
                     branch, branch);
            return 1;
         }
      }
   }

   /* 3. Test commands. Leading-invocation match so `grep 'make test' README`
    * does not trip the heuristic. */
   {
      static const struct
      {
         const char *match;
         const char *label;
      } test_cmds[] = {{"make unit-tests", "make unit-tests"},
                       {"make check", "make check"},
                       {"make test", "make test"},
                       {"npm test", "npm test"},
                       {"pnpm test", "pnpm test"},
                       {"yarn test", "yarn test"},
                       {"cargo test", "cargo test"},
                       {"go test", "go test"},
                       {"pytest", "pytest"},
                       {"ctest", "ctest"},
                       {NULL, NULL}};
      for (int i = 0; test_cmds[i].match; i++)
      {
         const char *p = wl_skip_ws(command);
         if (strncmp(p, test_cmds[i].match, strlen(test_cmds[i].match)) == 0)
         {
            char end = p[strlen(test_cmds[i].match)];
            if (end == '\0' || end == ' ' || end == '\t' || end == ';' || end == '\n')
            {
               snprintf(signal_type_out, stsize, "test-command");
               snprintf(rule_out, rsize, "Test command: `%s`", test_cmds[i].label);
               return 1;
            }
         }
      }
      /* Also accept `cd <dir> && <test>` — common aimee pattern */
      if (wl_cmd_has_word(command, "make") &&
          (strstr(command, "make unit-tests") || strstr(command, "make check") ||
           strstr(command, "make test")))
      {
         const char *label = strstr(command, "make unit-tests") ? "make unit-tests"
                             : strstr(command, "make check")    ? "make check"
                                                                : "make test";
         snprintf(signal_type_out, stsize, "test-command");
         snprintf(rule_out, rsize, "Test command: `%s`", label);
         return 1;
      }
   }

   return 0;
}

/* Determine a workspace label for the given directory by walking the
 * configured workspace roots. Returns 1 on match; out is always NUL-
 * terminated. */
static int wl_workspace_for_cwd(const char *cwd, char *out, size_t outlen)
{
   if (!cwd || !out || outlen == 0)
      return 0;
   out[0] = '\0';


   for (int i = 0; i < config_workspace_count(); i++)
   {
      size_t wlen = strlen(config_workspaces(i));
      if (wlen == 0)
         continue;
      if (strncmp(cwd, config_workspaces(i), wlen) == 0 && (cwd[wlen] == '/' || cwd[wlen] == '\0'))
      {
         const char *slash = strrchr(config_workspaces(i), '/');
         const char *name = slash ? slash + 1 : config_workspaces(i);
         snprintf(out, outlen, "%s", name);
         return 1;
      }
   }
   return 0;
}

void workflow_observe_bash(const char *command)
{
   if (!command || !command[0])
      return;

   char sig[64], rule[512];
   if (!workflow_parse_bash_signal(command, sig, sizeof(sig), rule, sizeof(rule)))
      return;

   char cwd[MAX_PATH_LEN];
   if (!getcwd(cwd, sizeof(cwd)))
      return;

   char ws[128];
   if (!wl_workspace_for_cwd(cwd, ws, sizeof(ws)))
      return;

   int64_t id = kb_client_memory_upsert_workflow(ws, sig, rule, 0.6, "post_tool_update");
   if (id > 0)
   {
      LOG_INFO("workflow_learn", "learned workflow rule: workflow:%s:%s", ws, sig);
      learning_implicit_record_workflow(ws, sig, rule);
   }
}
