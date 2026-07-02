/* wfe_native_gate.c -- see wfe_native_gate.h. Pure policy, no DB/engine deps. */
#include "wfe_native_gate.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "wfe_externalization.h" /* wfe_is_externalization_tool */

static int eq_ci(const char *a, const char *b)
{
   if (!a || !b)
      return 0;
   while (*a && *b)
   {
      if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
         return 0;
      a++;
      b++;
   }
   return *a == *b;
}

int wfe_is_shell_tool(const char *tool_name)
{
   if (!tool_name || !tool_name[0])
      return 0;
   static const char *const shells[] = {"bash",    "sh",   "shell",        "run_command",
                                        "command", "exec", "execute",      "terminal",
                                        "run",     "zsh",  "shell_command"};
   for (size_t i = 0; i < sizeof(shells) / sizeof(shells[0]); i++)
      if (eq_ci(tool_name, shells[i]))
         return 1;
   return 0;
}

static int name_is_web_tool(const char *tool_name)
{
   static const char *const web[] = {"webfetch", "websearch", "web_fetch",   "web_search",
                                     "fetch",    "browse",    "http_request"};
   for (size_t i = 0; i < sizeof(web) / sizeof(web[0]); i++)
      if (eq_ci(tool_name, web[i]))
         return 1;
   return 0;
}

/* Word-boundary-aware, case-insensitive search for `pat` in `hay`: the match must
 * start at the string start or after a shell separator/space so `git pushd` or a
 * path fragment does not trip a `git push` pattern. Intra-pattern spaces match one
 * or more whitespace runs so `git   push` still matches. */
static int cmd_has(const char *hay, const char *pat)
{
   if (!hay || !pat || !pat[0])
      return 0;
   for (const char *p = hay; *p; p++)
   {
      /* boundary before the candidate match */
      if (p != hay)
      {
         char prev = p[-1];
         if (!(prev == ' ' || prev == '\t' || prev == '\n' || prev == ';' || prev == '|' ||
               prev == '&' || prev == '(' || prev == '`' || prev == '{'))
            continue;
      }
      const char *h = p;
      const char *q = pat;
      int ok = 1;
      while (*q)
      {
         if (*q == ' ')
         {
            if (!(*h == ' ' || *h == '\t'))
            {
               ok = 0;
               break;
            }
            while (*h == ' ' || *h == '\t')
               h++;
            q++;
            continue;
         }
         if (tolower((unsigned char)*h) != tolower((unsigned char)*q))
         {
            ok = 0;
            break;
         }
         h++;
         q++;
      }
      if (ok)
      {
         /* Trailing boundary: a pattern not ending in space must not be a prefix of
          * a longer token (`git push` must not match `git pushd`). A pattern ending
          * in space already consumed a whitespace boundary. */
         size_t plen = strlen(pat);
         if (plen > 0 && pat[plen - 1] == ' ')
            return 1;
         char after = *h;
         if (after == '\0' || after == ' ' || after == '\t' || after == '\n' || after == ';' ||
             after == '|' || after == '&' || after == ')' || after == '`' || after == '\'' ||
             after == '"' || after == '/')
            return 1;
         /* else: matched a longer token's prefix -> keep scanning for a real match */
      }
   }
   return 0;
}

/* A URL/host that is clearly NOT loopback. Finds http(s):// and checks the host is
 * not localhost / 127.* / 0.0.0.0 / [::1] / ::1. Conservative: an unrecognised or
 * absent scheme returns 0 (fetchers with only a local target are not flagged). */
static int has_external_url(const char *cmd)
{
   if (!cmd)
      return 0;
   const char *schemes[] = {"http://", "https://", "ftp://"};
   for (size_t s = 0; s < sizeof(schemes) / sizeof(schemes[0]); s++)
   {
      const char *u = cmd;
      while ((u = strstr(u, schemes[s])) != NULL)
      {
         const char *host = u + strlen(schemes[s]);
         /* loopback hosts */
         if (strncmp(host, "localhost", 9) == 0 || strncmp(host, "127.", 4) == 0 ||
             strncmp(host, "0.0.0.0", 7) == 0 || strncmp(host, "[::1]", 5) == 0 ||
             strncmp(host, "::1", 3) == 0)
         {
            u = host;
            continue;
         }
         if (host[0] && host[0] != '/' && host[0] != ' ')
            return 1; /* a non-loopback host follows the scheme */
         u = host;
      }
   }
   return 0;
}

int wfe_native_tool_externalizes(const char *tool_name, const char *command)
{
   if (!tool_name || !tool_name[0])
      return 0;

   /* Named externalization primitives (aimee-MCP tools) + web/egress tools. */
   if (wfe_is_externalization_tool(tool_name) || name_is_web_tool(tool_name))
      return 1;

   if (!wfe_is_shell_tool(tool_name) || !command || !command[0])
      return 0;

   /* git remote writes */
   if (cmd_has(command, "git push") || cmd_has(command, "git send-email") ||
       cmd_has(command, "git remote add") || cmd_has(command, "git remote set-url"))
      return 1;

   /* gh MUTATIONS only (read subcommands like `gh pr view/list` must not trip). */
   static const char *const gh[] = {"gh pr create",      "gh pr merge",       "gh pr comment",
                                    "gh pr edit",        "gh pr ready",       "gh pr close",
                                    "gh release create", "gh release upload", "gh issue create",
                                    "gh issue comment",  "gh issue close",    "gh api "};
   for (size_t i = 0; i < sizeof(gh) / sizeof(gh[0]); i++)
      if (cmd_has(command, gh[i]))
         return 1;

   /* package / artifact publishes */
   static const char *const pub[] = {"npm publish",   "yarn publish",  "pnpm publish",
                                     "cargo publish", "twine upload",  "docker push",
                                     "gem push",      "poetry publish"};
   for (size_t i = 0; i < sizeof(pub) / sizeof(pub[0]); i++)
      if (cmd_has(command, pub[i]))
         return 1;

   /* remote transfer / remote exec (conservative -- may over-match a local target;
    * the WARN stage surfaces false positives before ENFORCE). */
   if (cmd_has(command, "scp ") || cmd_has(command, "sftp ") || cmd_has(command, "rsync ") ||
       cmd_has(command, "ssh "))
      return 1;

   /* network fetchers targeting an explicit non-loopback URL */
   if ((cmd_has(command, "curl") || cmd_has(command, "wget") || cmd_has(command, "telnet") ||
        cmd_has(command, "ncat") || cmd_has(command, "nc ")) &&
       has_external_url(command))
      return 1;

   return 0;
}
