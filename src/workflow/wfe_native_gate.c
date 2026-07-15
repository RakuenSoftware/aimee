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

/* An unquoted shell control char: after any of these the next word is again in
 * COMMAND position. `\n` is here and must be tested BEFORE any isspace() skip --
 * it is both whitespace and a command separator, and treating it as mere spacing
 * hides `make build<newline>git push`. */
static int is_ctl(char c)
{
   return c == ';' || c == '&' || c == '|' || c == '(' || c == ')' || c == '{' || c == '}' ||
          c == '`' || c == '\n';
}

/* Read one shell WORD from *pp into buf, honoring quotes and backslash escapes, and
 * writing the UNQUOTED content. The word ends at unquoted whitespace or an unquoted
 * control char; quoted whitespace stays inside the word, so `'git push'` reads back
 * as the single word `git push`. Stripping the quotes also collapses the
 * split-name evasion (`gi''t` / `g"i"t` -> `git`). Advances *pp; returns the length
 * written. */
static size_t read_word(const char **pp, char *buf, size_t cap)
{
   const char *p = *pp;
   size_t n = 0;
   int sq = 0, dq = 0;
   while (*p)
   {
      char c = *p;
      if (!sq && c == '\\' && p[1])
      {
         p++;
         if (n + 1 < cap)
            buf[n++] = *p;
         p++;
         continue;
      }
      if (!dq && c == '\'')
      {
         sq = !sq;
         p++;
         continue;
      }
      if (!sq && c == '"')
      {
         dq = !dq;
         p++;
         continue;
      }
      if (!sq && !dq && (c == ' ' || c == '\t' || c == '\r'))
         break;
      if (!sq && !dq && is_ctl(c))
         break;
      if (n + 1 < cap)
         buf[n++] = c;
      p++;
   }
   buf[n] = '\0';
   *pp = p;
   return n;
}

static int word_in(const char *w, const char *const *set, size_t n)
{
   for (size_t i = 0; i < n; i++)
      if (strcmp(w, set[i]) == 0)
         return 1;
   return 0;
}

static const char *base_of(const char *w) /* /usr/bin/git -> git */
{
   const char *b = w;
   for (const char *q = w; *q; q++)
      if (*q == '/')
         b = q + 1;
   return b;
}

/* 1 if `cmd` INVOKES one of `names`, rather than merely mentioning it. Walks the
 * line tracking command position: a word is a command only at the start or after an
 * unquoted control char. `git push`, `/usr/bin/git push`, `sudo git push`,
 * `VAR=1 git push` and `make && git push` match; `grep "git" f` and `echo git` do
 * not -- there the word is an argument. `bash -c '<cmd>'` recurses into the command
 * string, which is where the quoted-evasion case is actually caught (rather than by
 * treating every quote as a separator, which would deny `grep "git" f` too).
 *
 * KNOWN LIMITS, deliberately not chased (the header's honest-scope note applies):
 * a wrapper's own option ARGUMENT is not modelled -- `env -i git push` matches
 * because bare `-i` is skipped, but `sudo -u bob git push` does not, since `bob` is
 * read as the command. A local script named `git`/`gh` earlier in PATH matches on
 * name alone. Obfuscation the string cannot see (base64, env indirection, a
 * variable holding the binary name) is out of scope -- the stripped credential, not
 * this parser, is what closes those. */
static int scan_cmd(const char *cmd, const char *const *names, size_t nnames, int depth)
{
   if (!cmd || !cmd[0] || depth > 2)
      return 0;
   static const char *const prefixes[] = {"sudo", "env",  "command", "nohup",
                                          "time", "doas", "exec",    "builtin"};
   static const char *const shells[] = {"bash", "sh", "zsh", "dash", "ksh", "ash"};
   int at_cmd = 1, shell_cmd = 0;
   const char *p = cmd;
   char w[1024];
   while (*p)
   {
      if (*p == ' ' || *p == '\t' || *p == '\r')
      {
         p++;
         continue;
      }
      if (is_ctl(*p))
      {
         at_cmd = 1;
         shell_cmd = 0;
         p++;
         continue;
      }
      const char *before = p;
      size_t n = read_word(&p, w, sizeof(w));
      if (p == before) /* defensive: never stall on an unconsumed char */
      {
         p++;
         continue;
      }
      if (n == 0)
         continue;

      if (at_cmd)
      {
         if (strchr(w, '=')) /* `VAR=val cmd` -> the next word is still the command */
            continue;
         if (w[0] == '-') /* a wrapper's own flag: `env -i git push` */
            continue;
         const char *b = base_of(w);
         if (word_in(b, prefixes, sizeof(prefixes) / sizeof(prefixes[0])))
            continue; /* `sudo git push` -> still at_cmd */
         if (word_in(b, names, nnames))
            return 1;
         shell_cmd = word_in(b, shells, sizeof(shells) / sizeof(shells[0]));
         at_cmd = 0; /* this word was the command; the rest are its arguments */
      }
      else if (shell_cmd && w[0] == '-' && strchr(w, 'c'))
      {
         /* `bash -c` / `-lc` / `-lic`: the NEXT word is a command string, not data. */
         while (*p == ' ' || *p == '\t')
            p++;
         char inner[2048];
         if (read_word(&p, inner, sizeof(inner)) > 0 &&
             scan_cmd(inner, names, nnames, depth + 1))
            return 1;
      }
   }
   return 0;
}

int wfe_shell_invokes_git(const char *tool_name, const char *command)
{
   if (!wfe_is_shell_tool(tool_name) || !command || !command[0])
      return 0;
   static const char *const names[] = {"git", "gh"};
   return scan_cmd(command, names, sizeof(names) / sizeof(names[0]), 0);
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
