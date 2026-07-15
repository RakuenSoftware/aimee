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

/* A shell WORD-continuation char: an unquoted `git` token is bounded by anything
 * that is not one of these. Defining the boundary as "not a word char" (rather than
 * enumerating separators) robustly handles `;`, `}`, tabs, quotes, `/usr/bin/git`,
 * `bash -lc 'git push'`, etc. -- consult #982 roundtable [0][1][2][8][11][14][16]. */
static int is_word_char(char c)
{
   return isalnum((unsigned char)c) || c == '_';
}

/* Word-boundary-aware, case-insensitive search for `pat` in `hay`: the match must be
 * bounded by non-word chars on both sides so `git pushd` / `mygit push` do not trip a
 * `git push` pattern. Intra-pattern spaces match one or more whitespace runs so
 * `git   push` still matches. */
static int cmd_has(const char *hay, const char *pat)
{
   if (!hay || !pat || !pat[0])
      return 0;
   for (const char *p = hay; *p; p++)
   {
      /* leading boundary: start of string, or the previous char is not a word char */
      if (p != hay && is_word_char(p[-1]))
         continue;
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
         if ((plen > 0 && pat[plen - 1] == ' ') || !is_word_char(*h))
            return 1;
         /* else: matched a longer token's prefix -> keep scanning for a real match */
      }
   }
   return 0;
}

static int is_loopback_host(const char *h, const char *end)
{
   size_t n = (size_t)(end - h);
   if (n >= 9 && strncmp(h, "localhost", 9) == 0 && (n == 9 || h[9] == ':'))
      return 1;
   if (n >= 4 && strncmp(h, "127.", 4) == 0)
      return 1;
   if (n >= 7 && strncmp(h, "0.0.0.0", 7) == 0)
      return 1;
   if (n >= 5 && strncmp(h, "[::1]", 5) == 0)
      return 1;
   if (n >= 3 && strncmp(h, "::1", 3) == 0)
      return 1;
   return 0;
}

/* A URL whose HOST is clearly NOT loopback. Finds http(s)/ftp://, isolates the
 * authority (up to the next / ? # or whitespace/quote), skips any userinfo before an
 * '@' (so http://localhost@evil.com is judged on evil.com -- consult [13]), and
 * checks the host against localhost/127./0.0.0.0/::1/[::1]. An IP-encoded loopback
 * (0x7f000001) is judged non-loopback = flagged, which over-blocks (safe side). A
 * scheme-less target (nc host 443) is NOT seen here -- documented residual. */
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
         const char *auth = u + strlen(schemes[s]);
         const char *end = auth;
         while (*end && *end != '/' && *end != '?' && *end != '#' && *end != ' ' && *end != '\t' &&
                *end != '\n' && *end != '"' && *end != '\'' && *end != '|' && *end != ')' &&
                *end != '`')
            end++;
         const char *host = auth; /* skip userinfo: host is after the last '@' */
         for (const char *q = auth; q < end; q++)
            if (*q == '@')
               host = q + 1;
         if (host < end && !is_loopback_host(host, end))
            return 1;
         u = end;
      }
   }
   return 0;
}

/* Shell characters after which the next word sits in COMMAND position. Quote marks
 * are deliberately included: it makes `bash -lc 'git push'` read as a git
 * invocation -- the obvious evasion -- at the cost of also flagging `echo 'git'`.
 * That false positive is an accepted trade: this gate fails closed and its message
 * names the tool to use instead, whereas a miss lets a delegate push with a
 * credential it should never have touched. */
static int is_cmd_start(char c)
{
   return c == ';' || c == '&' || c == '|' || c == '(' || c == ')' || c == '{' || c == '}' ||
          c == '\n' || c == '`' || c == '\'' || c == '"';
}

/* 1 if `cmd` INVOKES one of `names` as a command, rather than merely mentioning it:
 * `git push` and `/usr/bin/git push` and `sudo git push` match; `grep git file` and
 * `echo git` do not (there `git` is an argument). A leading path is reduced to its
 * basename, and command-prefix words (sudo/env/...) and VAR=val assignments are
 * looked through so the real command is still reached. */
static int shell_invokes(const char *cmd, const char *const *names, size_t nnames)
{
   if (!cmd)
      return 0;
   static const char *const prefixes[] = {"sudo", "env",  "command", "nohup",
                                          "time", "doas", "exec",    "builtin"};
   int at_cmd = 1;
   const char *p = cmd;
   while (*p)
   {
      if (isspace((unsigned char)*p))
      {
         p++;
         continue;
      }
      if (is_cmd_start(*p))
      {
         at_cmd = 1;
         p++;
         continue;
      }
      const char *s = p;
      while (*p && !isspace((unsigned char)*p) && !is_cmd_start(*p))
         p++;
      if (!at_cmd)
         continue; /* an argument of the command already seen */

      const char *b = s; /* basename: /usr/bin/git -> git */
      for (const char *q = s; q < p; q++)
         if (*q == '/')
            b = q + 1;
      size_t blen = (size_t)(p - b);
      if (blen == 0)
      {
         at_cmd = 0;
         continue;
      }

      /* `VAR=val cmd` — an assignment prefix keeps the next word in command position. */
      int assign = 0;
      for (const char *q = s; q < p; q++)
         if (*q == '=')
         {
            assign = 1;
            break;
         }
      if (assign)
         continue;

      int looked_through = 0;
      for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
         if (strlen(prefixes[i]) == blen && strncmp(b, prefixes[i], blen) == 0)
         {
            looked_through = 1;
            break;
         }
      if (looked_through)
         continue; /* still at_cmd: `sudo git push` */

      for (size_t i = 0; i < nnames; i++)
         if (strlen(names[i]) == blen && strncmp(b, names[i], blen) == 0)
            return 1;
      at_cmd = 0; /* this word was the command; what follows are its arguments */
   }
   return 0;
}

int wfe_shell_invokes_git(const char *tool_name, const char *command)
{
   if (!wfe_is_shell_tool(tool_name) || !command || !command[0])
      return 0;
   static const char *const names[] = {"git", "gh"};
   return shell_invokes(command, names, sizeof(names) / sizeof(names[0]));
}

int wfe_native_tool_forbidden(const char *tool_name, const char *command)
{
   if (!wfe_is_shell_tool(tool_name) || !command || !command[0])
      return 0;

   /* An admin override of branch protection is human-only. `--admin` is only
    * meaningful to `gh pr merge`, but we do not require the two to be adjacent:
    * flags may precede the subcommand, and a compound line may carry both. Any
    * shell line that both merges a PR and asks for the admin bypass is denied. */
   if (cmd_has(command, "gh pr merge") &&
       (cmd_has(command, "--admin") || cmd_has(command, "-admin")))
      return 1;

   /* The `gh api` equivalent: a PUT to .../pulls/<n>/merge. `gh api` is otherwise a
    * documented accepted residual (see wfe_native_tool_externalizes), but the merge
    * endpoint is the same bypass wearing a different hat when the token is an admin's. */
   if (cmd_has(command, "gh api") && strstr(command, "/merge") &&
       (cmd_has(command, "-X PUT") || cmd_has(command, "--method PUT")))
      return 1;

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

   /* gh MUTATIONS only (read subcommands like `gh pr view/list` must not trip).
    * `gh api` is deliberately NOT matched: it is a GET by default (read), and its
    * mutating -X POST form is an accepted, documented residual (over-narrowing here
    * would false-positive on the common read case). */
   static const char *const gh[] = {"gh pr create",      "gh pr merge",       "gh pr comment",
                                    "gh pr edit",        "gh pr ready",       "gh pr close",
                                    "gh release create", "gh release upload", "gh issue create",
                                    "gh issue comment",  "gh issue close"};
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

wfe_native_decision_t wfe_native_gate_decision(int externalizes, int bound, int delivered,
                                               int stage_hard)
{
   if (!externalizes || !bound || delivered)
      return WFE_NATIVE_ALLOW;
   return stage_hard ? WFE_NATIVE_DENY : WFE_NATIVE_WARN;
}
