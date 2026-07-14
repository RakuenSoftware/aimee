/* util.c: core utilities (normalization, option parsing, security, path helpers) */
#include "aimee.h"
#include "headers/util.h"
#include <ctype.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#ifndef _WIN32
/* POSIX process helpers (run_cmd*) live below; their headers and the `environ`
 * declaration are unavailable on Windows (MinGW has no <sys/wait.h>). The thin
 * Windows client does not use run_cmd, so the whole block is POSIX-only. */
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#endif

/* --- Shared utilities (used by all modules including tests) --- */

char *safe_strdup(const char *s)
{
   if (!s)
      return NULL;
   char *dup = strdup(s);
   if (!dup)
      fatal("out of memory (strdup)");
   return dup;
}

static const char *find_ci_local(const char *haystack, const char *needle)
{
   if (!haystack || !needle || !needle[0])
      return NULL;
   size_t nlen = strlen(needle);
   for (const char *p = haystack; *p; p++)
   {
      size_t i = 0;
      while (i < nlen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
         i++;
      if (i == nlen)
         return p;
   }
   return NULL;
}

char *strip_llm_private_scaffold(const char *text)
{
   const char *p = text ? text : "";
   while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t' || *p == '*' || *p == '-' ||
          *p == '"' || *p == '\'')
      p++;
   if (strncasecmp(p, "Thinking Process", 16) != 0 && strncasecmp(p, "Self-Correction", 15) != 0 &&
       strncasecmp(p, "Thought Process", 15) != 0)
      return safe_strdup(text ? text : "");
   const char *marker = find_ci_local(text, "Final Output");
   if (!marker)
      marker = find_ci_local(text, "Final Answer");
   if (!marker)
      return safe_strdup("");
   p = strchr(marker, '\n');
   if (!p)
      return safe_strdup("");
   p++;
   while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t' || *p == '*' || *p == '-' ||
          *p == '"' || *p == '\'')
      p++;
   char *out = safe_strdup(p);
   size_t len = strlen(out);
   while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' || out[len - 1] == ' ' ||
                      out[len - 1] == '\t' || out[len - 1] == '"'))
      out[--len] = '\0';
   return out;
}

/* A line carries AI attribution if, after leading whitespace, it starts with a
 * "co-authored-by:" trailer, or it contains the markdown-link attribution
 * "generated with [claude"/"generated with [codex" anywhere. Case-insensitive.
 * Mirrors the anchored regex in .github/workflows/ci.yml (ai-attribution gate)
 * so Aimee never emits what CI rejects. `end` bounds the line (exclusive). */
static int line_is_ai_attribution(const char *line, const char *end)
{
   const char *p = line;
   while (p < end && (*p == ' ' || *p == '\t'))
      p++;
   if (end - p >= 15 && strncasecmp(p, "co-authored-by:", 15) == 0)
      return 1;
   for (const char *q = line; end - q >= 16 + 5; q++)
   {
      if (strncasecmp(q, "generated with [", 16) != 0)
         continue;
      const char *r = q + 16;
      if ((end - r >= 6 && strncasecmp(r, "claude", 6) == 0) ||
          (end - r >= 5 && strncasecmp(r, "codex", 5) == 0))
         return 1;
   }
   return 0;
}

int strip_ai_attribution(char *text)
{
   if (!text)
      return 0;
   int removed = 0;
   char *src = text, *dst = text;
   while (*src)
   {
      char *eol = strchr(src, '\n');
      char *next = eol ? eol + 1 : src + strlen(src);
      const char *end = eol ? eol : next;
      if (line_is_ai_attribution(src, end))
         removed++;
      else
      {
         size_t n = (size_t)(next - src);
         memmove(dst, src, n);
         dst += n;
      }
      src = next;
   }
   *dst = '\0';
   if (removed)
   {
      size_t len = (size_t)(dst - text);
      while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r' || text[len - 1] == ' ' ||
                         text[len - 1] == '\t'))
         text[--len] = '\0';
   }
   return removed;
}

void fatal(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   fprintf(stderr, "aimee: ");
   vfprintf(stderr, fmt, ap);
   fprintf(stderr, "\n");
   va_end(ap);
   exit(1);
}

void now_utc(char *buf, size_t len)
{
   time_t t = time(NULL);
   struct tm tm;
   gmtime_r(&t, &tm);
   strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* --- Filler words for normalization --- */

static const char *filler_words[] = {"the",   "a",     "an",    "is",    "are",      "was",
                                     "were",  "be",    "uh",    "um",    "actually", "well",
                                     "think", "maybe", "kinda", "sorta", NULL};

static int is_filler(const char *word)
{
   for (int i = 0; filler_words[i]; i++)
   {
      if (strcmp(word, filler_words[i]) == 0)
         return 1;
   }
   return 0;
}

static void normalize_strip_possessive(char *word)
{
   size_t len;
   if (!word || !word[0])
      return;
   len = strlen(word);
   if (len > 2 && word[len - 2] == '\'' && word[len - 1] == 's')
   {
      word[len - 2] = '\0';
      return;
   }
   if (len > 2 && word[len - 1] == '\'' && word[len - 2] == 's')
      word[len - 1] = '\0';
}

static int normalize_is_month_name_at(const char *s)
{
   static const char *months[] = {
       "jan", "january", "feb", "february", "mar", "march",    "apr", "april", "may",
       "jun", "june",    "jul", "july",     "aug", "august",   "sep", "sept",  "september",
       "oct", "october", "nov", "november", "dec", "december", NULL};
   if (!s || !isalpha((unsigned char)s[0]))
      return 0;
   for (int i = 0; months[i]; i++)
   {
      size_t len = strlen(months[i]);
      if (strncasecmp(s, months[i], len) == 0 && !isalpha((unsigned char)s[len]))
         return 1;
   }
   return 0;
}

/* --- normalize_key --- */

char *normalize_key(const char *key, char *buf, size_t buf_len)
{
   if (!key || !buf || buf_len == 0)
   {
      if (buf && buf_len > 0)
         buf[0] = '\0';
      return buf;
   }

   /* Lowercase copy. Strip date wrappers and split dashed dates without
    * changing ordinary hyphenated keys like integ-test. */
   char tmp[4096];
   size_t ki = 0;
   for (size_t i = 0; key[i] && ki < sizeof(tmp) - 1; i++)
   {
      unsigned char ch = (unsigned char)key[i];
      if (isalnum(ch))
         tmp[ki++] = (char)tolower(ch);
      else if (isspace(ch) || ch == '[' || ch == ']' || ch == '(' || ch == ')' || ch == '{' ||
               ch == '}')
         tmp[ki++] = ' ';
      else if (ch == '-')
      {
         unsigned char prev = i > 0 ? (unsigned char)tolower((unsigned char)key[i - 1]) : 0;
         const char *next = key[i + 1] ? key + i + 1 : NULL;
         int split_dash = 0;
         if (isdigit(prev) && next && isdigit((unsigned char)next[0]))
            split_dash = 1;
         else if (isdigit(prev) && normalize_is_month_name_at(next))
            split_dash = 1;
         else if (i >= 3 && isdigit((unsigned char)key[i + 1]))
         {
            const char *start = key + i - 1;
            while (start > key && isalpha((unsigned char)start[-1]))
               start--;
            if (normalize_is_month_name_at(start))
               split_dash = 1;
         }
         tmp[ki++] = split_dash ? ' ' : '-';
      }
      else
         tmp[ki++] = (char)tolower(ch);
   }
   tmp[ki] = '\0';

   /* Split into words, skip filler, collapse whitespace */
   size_t out = 0;
   char *saveptr;
   char *tok = strtok_r(tmp, " \t\n\r", &saveptr);
   while (tok)
   {
      normalize_strip_possessive(tok);
      if (!is_filler(tok))
      {
         if (out > 0 && out < buf_len - 1)
            buf[out++] = ' ';
         size_t wlen = strlen(tok);
         if (out + wlen < buf_len)
         {
            memcpy(buf + out, tok, wlen);
            out += wlen;
         }
      }
      tok = strtok_r(NULL, " \t\n\r", &saveptr);
   }
   buf[out] = '\0';
   return buf;
}

static int is_laughter_token(const char *s)
{
   if (!s || !s[0])
      return 0;
   size_t len = strlen(s);
   if (len < 3)
      return 0;
   for (size_t i = 0; i < len; i++)
   {
      char ch = (char)tolower((unsigned char)s[i]);
      if (ch != 'h' && ch != 'a')
         return 0;
   }
   return strchr(s, 'h') != NULL && strchr(s, 'a') != NULL;
}

int is_noise_utterance(const char *text)
{
   static const char *exact[] = {"ok",      "okay",   "k",       "kk",          "sure",
                                 "yeah",    "yep",    "yup",     "nope",        "lol",
                                 "lmao",    "omg",    "nice",    "cool",        "sounds good",
                                 "sg",      "got it", "works",   "thanks",      "thank you",
                                 "perfect", "great",  "awesome", "makes sense", NULL};
   static const char *prefix[] = {"ok ",   "okay ",  "sure ",    "yeah ",   "yep ",   "yup ",
                                  "lol ",  "lmao ",  "omg ",     "thanks ", "thank ", "nice ",
                                  "cool ", "great ", "awesome ", NULL};
   if (!text || !text[0])
      return 0;

   char norm[512];
   normalize_key(text, norm, sizeof(norm));
   if (!norm[0])
      return 1;

   for (int i = 0; exact[i]; i++)
   {
      if (strcmp(norm, exact[i]) == 0)
         return 1;
   }
   for (int i = 0; prefix[i]; i++)
   {
      size_t len = strlen(prefix[i]);
      if (strncmp(norm, prefix[i], len) == 0)
         return 1;
   }

   if (is_laughter_token(norm))
      return 1;

   int saw_alnum = 0;
   int saw_alpha = 0;
   int punctuation_only = 1;
   for (const unsigned char *p = (const unsigned char *)text; *p; p++)
   {
      if (isalnum(*p))
      {
         saw_alnum = 1;
         punctuation_only = 0;
         if (isalpha(*p))
            saw_alpha = 1;
      }
      else if (!isspace(*p) && !ispunct(*p))
      {
         punctuation_only = 0;
      }
   }
   if (!saw_alnum || punctuation_only)
      return 1;

   char *tokens[8] = {0};
   int count = tokenize_for_search(text, tokens, 8);
   if (count <= 0)
      return 1;

   int low_signal = count <= 2;
   int all_short = 1;
   for (int i = 0; i < count; i++)
   {
      if ((int)strlen(tokens[i]) > 4)
         all_short = 0;
      free(tokens[i]);
   }

   if (low_signal && all_short && saw_alpha && strlen(norm) <= 16)
      return 1;

   return 0;
}

/* --- shlex_split --- */

int shlex_split(const char *command, char **out, int max_tokens)
{
   if (!command || !out || max_tokens <= 0)
      return 0;

   int count = 0;
   const char *p = command;

   while (*p && count < max_tokens)
   {
      /* Skip whitespace */
      while (*p == ' ' || *p == '\t')
         p++;
      if (!*p)
         break;

      /* Check for digit-prefixed redirections like 2> */
      if (*p == '2' && p[1] == '>')
      {
         char op[4] = {'2', '>', '\0', '\0'};
         if (p[2] == '>')
         {
            op[2] = '>';
            p += 3;
         }
         else
         {
            p += 2;
         }
         out[count] = strdup(op);
         if (!out[count])
            break;
         count++;
         continue;
      }

      /* Check for shell operators */
      if (*p == '|' || *p == '>' || *p == '<' || *p == '&' || *p == ';')
      {
         char op[4] = {*p, '\0', '\0', '\0'};
         if (p[1] == p[0]) /* ||, >>, &&, etc. */
         {
            op[1] = p[1];
            p += 2;
         }
         else if (*p == '>' && p[1] == '&')
         {
            op[1] = '&';
            p += 2;
         }
         else
         {
            p++;
         }
         out[count] = strdup(op);
         if (!out[count])
            break;
         count++;
         continue;
      }

      /* Build token */
      char token[MAX_PATH_LEN];
      size_t ti = 0;
      char quote = 0;

      while (*p && ti < sizeof(token) - 1)
      {
         if (quote)
         {
            if (*p == quote)
            {
               quote = 0;
               p++;
            }
            else if (*p == '\\' && quote == '"' && p[1])
            {
               p++;
               token[ti++] = *p++;
            }
            else
            {
               token[ti++] = *p++;
            }
         }
         else
         {
            if (*p == '\'' || *p == '"')
            {
               quote = *p++;
            }
            else if (*p == '\\' && p[1])
            {
               p++;
               token[ti++] = *p++;
            }
            else if (*p == ' ' || *p == '\t' || *p == '|' || *p == '>' || *p == '<' || *p == '&' ||
                     *p == ';')
            {
               break;
            }
            else
            {
               token[ti++] = *p++;
            }
         }
      }
      token[ti] = '\0';

      if (ti > 0)
      {
         out[count] = strdup(token);
         if (!out[count])
            break;
         count++;
      }
   }

   return count;
}

/* --- is_likely_path --- */

int is_likely_path(const char *tok)
{
   if (!tok)
      return 0;
   if (tok[0] == '/')
      return 1;
   if (tok[0] == '.' && tok[1] == '/')
      return 1;
   if (tok[0] == '.' && tok[1] == '.' && tok[2] == '/')
      return 1;
   if (tok[0] == '~' && tok[1] == '/')
      return 1;

   /* Check for file extension patterns */
   const char *dot = strrchr(tok, '.');
   if (dot && strchr(tok, '/'))
      return 1;

   return 0;
}

/* --- extract_paths_shlex --- */

int extract_paths_shlex(const char *command, char **out, int max_paths)
{
   char *tokens[256];
   int tc = shlex_split(command, tokens, 256);

   int count = 0;
   for (int i = 0; i < tc && count < max_paths; i++)
   {
      if (is_likely_path(tokens[i]))
      {
         out[count] = strdup(tokens[i]);
         count++;
      }
   }

   /* Free all tokens */
   for (int i = 0; i < tc; i++)
      free(tokens[i]);

   return count;
}

/* --- Option parsing --- */

static int is_bool_flag(const char *name, const char **bool_flags)
{
   if (!bool_flags)
      return 0;
   for (int i = 0; bool_flags[i]; i++)
   {
      if (strcmp(name, bool_flags[i]) == 0)
         return 1;
   }
   return 0;
}

void opt_parse(int argc, char **argv, const char **bool_flags, opt_parsed_t *out)
{
   memset(out, 0, sizeof(*out));

   for (int i = 0; i < argc; i++)
   {
      if (strncmp(argv[i], "--", 2) == 0 && argv[i][2] != '\0')
      {
         const char *flag = argv[i] + 2;
         const char *eq = strchr(flag, '=');

         if (out->flag_count >= OPT_MAX_FLAGS)
            continue;

         opt_flag_t *f = &out->flags[out->flag_count];
         if (eq)
         {
            /* --flag=value: name includes chars up to '=' */
            f->name = flag;
            f->value = eq + 1;
            f->is_bool = 0;
         }
         else if (is_bool_flag(flag, bool_flags))
         {
            f->name = flag;
            f->value = "";
            f->is_bool = 1;
         }
         else if (i + 1 < argc && argv[i + 1][0] != '-')
         {
            /* --flag value */
            f->name = flag;
            f->value = argv[++i];
            f->is_bool = 0;
         }
         else
         {
            /* Treat as bool */
            f->name = flag;
            f->value = "";
            f->is_bool = 1;
         }
         out->flag_count++;
      }
      else
      {
         if (out->pos_count < OPT_MAX_POSITIONAL)
            out->positional[out->pos_count++] = argv[i];
      }
   }
}

const char *opt_get(const opt_parsed_t *opts, const char *name)
{
   size_t nlen = strlen(name);
   for (int i = 0; i < opts->flag_count; i++)
   {
      const char *fn = opts->flags[i].name;
      if (strncmp(fn, name, nlen) == 0 && (fn[nlen] == '\0' || fn[nlen] == '='))
         return opts->flags[i].value;
   }
   return NULL;
}

int opt_has(const opt_parsed_t *opts, const char *name)
{
   return opt_get(opts, name) != NULL;
}

const char *opt_pos(const opt_parsed_t *opts, int index)
{
   if (index < 0 || index >= opts->pos_count)
      return NULL;
   return opts->positional[index];
}

int opt_get_int(const opt_parsed_t *opts, const char *name, int default_val)
{
   const char *v = opt_get(opts, name);
   return v ? atoi(v) : default_val;
}

int opt_get_flag(const opt_parsed_t *opts, const char *name)
{
   return opt_has(opts, name);
}

/* --- Command execution --- */

#define RUN_CMD_INIT_SIZE 65536

/* Per-thread working directory for run_cmd(). Set via run_cmd_set_cwd() so
 * concurrent threads in the server pool never race on the process-global CWD.
 * When set, run_cmd() prepends "cd '<dir>' && " to the command so the shell
 * child uses the right directory without calling chdir() on the thread. */
static __thread char tl_run_cwd[MAX_PATH_LEN];

void run_cmd_set_cwd(const char *cwd)
{
   if (cwd && cwd[0])
      snprintf(tl_run_cwd, sizeof(tl_run_cwd), "%s", cwd);
   else
      tl_run_cwd[0] = '\0';
}

const char *run_cmd_get_cwd(void)
{
   return tl_run_cwd[0] ? tl_run_cwd : NULL;
}

#ifndef _WIN32
static char *run_cmd_impl(const char *cmd, int *exit_code)
{
   FILE *fp = popen(cmd, "r");
   if (!fp)
   {
      *exit_code = -1;
      return NULL;
   }

   size_t cap = RUN_CMD_INIT_SIZE;
   char *buf = malloc(cap);
   if (!buf)
   {
      pclose(fp);
      *exit_code = -1;
      return NULL;
   }

   size_t total = 0;
   size_t n;
   while ((n = fread(buf + total, 1, cap - total - 1, fp)) > 0)
   {
      total += n;
      /* Grow buffer before it fills: realloc when less than 1KB of headroom remains */
      if (cap - total < 1024)
      {
         size_t new_cap = cap * 2;
         char *new_buf = realloc(buf, new_cap);
         if (!new_buf)
         {
            /* Out of memory: drain the pipe so the child can exit, then bail */
            char drain[4096];
            while (fread(drain, 1, sizeof(drain), fp) > 0)
               ;
            break;
         }
         buf = new_buf;
         cap = new_cap;
      }
   }
   buf[total] = '\0';

   int status = pclose(fp);
   *exit_code = platform_pclose_status(status);
   return buf;
}

char *run_cmd(const char *cmd, int *exit_code)
{
   if (!tl_run_cwd[0])
      return run_cmd_impl(cmd, exit_code);

   /* Prepend "cd '<dir>' && " so the shell child starts in the thread's
    * designated directory without mutating the process-global CWD. */
   char *esc = shell_escape(tl_run_cwd);
   size_t len = strlen(esc) + strlen(cmd) + 16;
   char *full = malloc(len);
   if (!full)
   {
      free(esc);
      return run_cmd_impl(cmd, exit_code);
   }
   snprintf(full, len, "cd %s && %s", esc, cmd);
   free(esc);
   char *result = run_cmd_impl(full, exit_code);
   free(full);
   return result;
}

char *run_cmd_env(const char *cmd, char *const envp[], int *exit_code)
{
   if (exit_code)
      *exit_code = -1;
   if (!cmd)
      return NULL;

   /* Compose the cwd-prefixed shell line, mirroring run_cmd(). */
   char *line = NULL;
   if (tl_run_cwd[0])
   {
      char *esc = shell_escape(tl_run_cwd);
      if (esc)
      {
         size_t len = strlen(esc) + strlen(cmd) + 16;
         line = malloc(len);
         if (line)
            snprintf(line, len, "cd %s && %s", esc, cmd);
         free(esc);
      }
   }
   const char *shcmd = line ? line : cmd;

   int pipefd[2];
   if (pipe(pipefd) != 0)
   {
      free(line);
      return NULL;
   }
   pid_t pid = fork();
   if (pid < 0)
   {
      close(pipefd[0]);
      close(pipefd[1]);
      free(line);
      return NULL;
   }
   if (pid == 0)
   {
      /* child: combined stdout+stderr -> pipe; exec sh -c under the given env.
       * The env (incl. any secret like GH_TOKEN) crosses ONLY here — never the
       * process environment, never the command line. */
      dup2(pipefd[1], STDOUT_FILENO);
      dup2(pipefd[1], STDERR_FILENO);
      close(pipefd[0]);
      close(pipefd[1]);
      char *const argv[] = {(char *)"sh", (char *)"-c", (char *)shcmd, NULL};
      execve("/bin/sh", argv, envp ? envp : environ);
      _exit(127);
   }
   close(pipefd[1]);

   size_t cap = RUN_CMD_INIT_SIZE, total = 0;
   char *buf = malloc(cap);
   if (buf)
   {
      ssize_t n;
      while ((n = read(pipefd[0], buf + total, cap - total - 1)) > 0)
      {
         total += (size_t)n;
         if (cap - total < 1024)
         {
            char *nb = realloc(buf, cap * 2);
            if (!nb)
            {
               char drain[4096];
               while (read(pipefd[0], drain, sizeof(drain)) > 0)
                  ;
               break;
            }
            buf = nb;
            cap *= 2;
         }
      }
      buf[total] = '\0';
   }
   close(pipefd[0]);

   int status = 0;
   waitpid(pid, &status, 0);
   *exit_code = platform_pclose_status(status);
   free(line);
   return buf;
}

int git_net_exec(const char *cwd, const char *const *git_argv, char **out_buf, size_t max_out)
{
   if (out_buf)
      *out_buf = NULL;
   if (!git_argv)
      return -1;

   size_t n = 0;
   while (git_argv[n])
      n++;

   /* argv: git <git_argv...> NULL. The repo is selected by chdir (cwd below). */
   const char **argv = malloc((1 + n + 1) * sizeof(*argv));
   if (!argv)
      return -1;
   size_t a = 0;
   argv[a++] = "git";
   for (size_t i = 0; i < n; i++)
      argv[a++] = git_argv[i];
   argv[a] = NULL;

   /* Child env: a copy of the parent environment with GIT_SSH_COMMAND and
    * GIT_TERMINAL_PROMPT FORCED to the safe values (any inherited copies dropped),
    * so the BatchMode/ConnectTimeout SSH command and no-prompt policy win
    * regardless of what the parent exported (git resolves the GIT_SSH_COMMAND env
    * above core.sshCommand config). PATH / HOME / SSH_AUTH_SOCK and vault tokens
    * are preserved — safe_exec_capture_*_env REPLACES the env, so a partial list
    * would strip them. Entries are borrowed (parent strings + string literals);
    * only the array is owned, so we free just the array. */
   size_t pn = 0;
   while (environ && environ[pn])
      pn++;
   char **envp = malloc((pn + 3) * sizeof(*envp)); /* parent + 2 forced + NULL */
   if (!envp)
   {
      free(argv);
      return -1;
   }
   size_t o = 0;
   for (size_t i = 0; i < pn; i++)
   {
      if (strncmp(environ[i], "GIT_SSH_COMMAND=", 16) == 0 ||
          strncmp(environ[i], "GIT_TERMINAL_PROMPT=", 20) == 0)
         continue;
      envp[o++] = environ[i];
   }
   envp[o++] = (char *)"GIT_SSH_COMMAND=" GIT_SAFE_SSH_COMMAND;
   envp[o++] = (char *)"GIT_TERMINAL_PROMPT=0";
   envp[o] = NULL;

   /* When no explicit cwd is given, fall back to the thread-local run_cmd CWD so
    * this is a drop-in for run_cmd("git ...") in server pool threads (which set
    * the repo dir there rather than passing -C). The wall-clock cap guarantees a
    * stalled remote can never hang the caller even if the SSH bound is bypassed. */
   const char *eff_cwd = (cwd && cwd[0]) ? cwd : run_cmd_get_cwd();
   char *out = NULL;
   int rc = safe_exec_capture_cwd_env_timeout((const char *const *)argv, eff_cwd, envp, &out,
                                              max_out ? max_out : 4096, GIT_NET_TIMEOUT_MS);
   free(envp);
   free(argv);
   if (out_buf)
      *out_buf = out;
   else
      free(out);
   return rc;
}
#endif /* !_WIN32 — run_cmd* are POSIX-only (fork/exec/waitpid) */

int has_shell_metachar(const char *s)
{
   if (!s)
      return 0;
   for (; *s; s++)
   {
      switch (*s)
      {
      case ';':
      case '|':
      case '&':
      case '$':
      case '`':
      case '(':
      case ')':
      case '{':
      case '}':
      case '<':
      case '>':
      case '\n':
      case '\r':
      case '\'':
      case '"':
      case '\\':
         return 1;
      }
   }
   return 0;
}

char *shell_escape(const char *raw)
{
   if (!raw)
      return strdup("");
   size_t len = strlen(raw);
   char *esc = malloc(len * 4 + 1);
   if (!esc)
      return strdup("");
   size_t j = 0;
   for (size_t i = 0; i < len; i++)
   {
      if (raw[i] == '\'')
      {
         esc[j++] = '\'';
         esc[j++] = '\\';
         esc[j++] = '\'';
         esc[j++] = '\'';
      }
      else
      {
         esc[j++] = raw[i];
      }
   }
   esc[j] = '\0';
   return esc;
}

int str_appendf(char *buf, int pos, int cap, const char *fmt, ...)
{
   if (!buf || pos < 0)
      return pos < 0 ? 0 : pos;
   if (pos >= cap)
      return cap;
   va_list ap;
   va_start(ap, fmt);
   int n = vsnprintf(buf + pos, (size_t)(cap - pos), fmt, ap);
   va_end(ap);
   if (n < 0)
      return pos;
   pos += n;
   return pos > cap ? cap : pos;
}

void sanitize_shell_token(char *s)
{
   if (!s)
      return;
   for (; *s; s++)
   {
      if (isalnum((unsigned char)*s) || *s == '-' || *s == '_' || *s == '.')
         continue;
      *s = '_';
   }
}

int is_safe_id(const char *s)
{
   if (!s || !s[0] || strlen(s) > 128)
      return 0;
   for (const char *p = s; *p; p++)
   {
      if (isalnum((unsigned char)*p) || *p == '-' || *p == '_')
         continue;
      return 0;
   }
   return 1;
}

/* delegation_error_guidance: append actionable fix guidance for known delegation errors.
 * Writes guidance into buf (up to len bytes). Returns 1 if guidance was found, 0 if not. */
int delegation_error_guidance(const char *error, char *buf, size_t len)
{
   if (!error || !buf || len == 0)
      return 0;

   static const struct
   {
      const char *pattern;
      const char *guidance;
   } table[] = {
       {"missing prompt", "\nFix: Provide a non-empty prompt describing the task."},
       {"prompt too short",
        "\nFix: Prompt is too brief — provide enough context for the delegate to work "
        "independently (at least 20 characters)."},
       {"no agent available for role",
        "\nFix: Use a valid role: code, review, explain, refactor, draft, summarize, deploy, "
        "validate, test, diagnose, execute. If using a custom role, ensure an agent is "
        "configured for it in agents.json."},
       {"missing delegation_id or content",
        "\nFix: Both 'delegation_id' and 'content' are required — add them to the "
        "delegate_reply call."},
       {"delegate creation denied",
        "\nFix: Only the primary agent may create delegates. A delegate should return its "
        "finding to the parent instead of spawning more delegates."},
       {"delegation depth limit exceeded",
        "\nFix: Reduce delegation nesting depth or increase max_delegation_depth in aimee "
        "config."},
       {"delegation spawn limit exceeded",
        "\nFix: The nested delegate budget for this root delegate has been reached. "
        "Reduce sub-delegation fan-out or increase max_delegation_spawns in aimee config."},
   };

   for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
   {
      if (strstr(error, table[i].pattern))
      {
         snprintf(buf, len, "%s", table[i].guidance);
         return 1;
      }
   }

   buf[0] = '\0';
   return 0;
}

/* ── Standard base64 (RFC 4648, with '=' padding) ───────────────────────────
 * Shared codec for binary-safe framing over text channels (e.g. raw PTY bytes
 * over an SSE/NDJSON stream). Linked into both the client and server binaries. */
static const char B64_ENC[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t aimee_base64_encoded_len(size_t in_len)
{
   return ((in_len + 2) / 3) * 4 + 1; /* incl. NUL */
}

size_t aimee_base64_encode(const unsigned char *in, size_t in_len, char *out, size_t out_cap)
{
   if (!out || out_cap == 0)
      return 0;
   if (aimee_base64_encoded_len(in_len) > out_cap)
   {
      out[0] = '\0';
      return 0;
   }
   size_t o = 0;
   size_t i = 0;
   for (; i + 3 <= in_len; i += 3)
   {
      unsigned v = (unsigned)in[i] << 16 | (unsigned)in[i + 1] << 8 | in[i + 2];
      out[o++] = B64_ENC[(v >> 18) & 0x3f];
      out[o++] = B64_ENC[(v >> 12) & 0x3f];
      out[o++] = B64_ENC[(v >> 6) & 0x3f];
      out[o++] = B64_ENC[v & 0x3f];
   }
   if (in_len - i == 1)
   {
      unsigned v = (unsigned)in[i] << 16;
      out[o++] = B64_ENC[(v >> 18) & 0x3f];
      out[o++] = B64_ENC[(v >> 12) & 0x3f];
      out[o++] = '=';
      out[o++] = '=';
   }
   else if (in_len - i == 2)
   {
      unsigned v = (unsigned)in[i] << 16 | (unsigned)in[i + 1] << 8;
      out[o++] = B64_ENC[(v >> 18) & 0x3f];
      out[o++] = B64_ENC[(v >> 12) & 0x3f];
      out[o++] = B64_ENC[(v >> 6) & 0x3f];
      out[o++] = '=';
   }
   out[o] = '\0';
   return o;
}

static int b64_val(int c)
{
   if (c >= 'A' && c <= 'Z')
      return c - 'A';
   if (c >= 'a' && c <= 'z')
      return c - 'a' + 26;
   if (c >= '0' && c <= '9')
      return c - '0' + 52;
   if (c == '+')
      return 62;
   if (c == '/')
      return 63;
   return -1;
}

/* Decode base64 `in` (NUL-terminated). Skips ASCII whitespace. Returns the
 * number of decoded bytes, or (size_t)-1 on a malformed character. */
size_t aimee_base64_decode(const char *in, unsigned char *out, size_t out_cap)
{
   if (!in || !out)
      return (size_t)-1;
   unsigned acc = 0;
   int bits = 0;
   size_t o = 0;
   for (const char *p = in; *p; p++)
   {
      if (*p == '=')
         break;
      if (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')
         continue;
      int v = b64_val((unsigned char)*p);
      if (v < 0)
         return (size_t)-1;
      acc = (acc << 6) | (unsigned)v;
      bits += 6;
      if (bits >= 8)
      {
         bits -= 8;
         if (o >= out_cap)
            return (size_t)-1;
         out[o++] = (unsigned char)((acc >> bits) & 0xff);
      }
   }
   return o;
}

/* --- Worktree isolation helpers (see headers/util.h) --- */

int aimee_path_is_main_clone(const char *path)
{
   if (!path || !path[0])
      return 0;
   char cur[MAX_PATH_LEN];
   if (snprintf(cur, sizeof(cur), "%s", path) >= (int)sizeof(cur))
      return 0; /* path truncated -> fail-open (allow), never scan a partial path */
   for (;;)
   {
      char g[MAX_PATH_LEN + 8];
      snprintf(g, sizeof(g), "%s/.git", cur);
      struct stat st;
      if (stat(g, &st) == 0)
         return S_ISDIR(st.st_mode) ? 1 : 0; /* dir = main clone; file = worktree */
      char *slash = strrchr(cur, '/');
      if (!slash || slash == cur)
         return 0;
      *slash = '\0';
   }
}

int aimee_edit_target_in_main_clone(const char *file_path, const char *cwd)
{
   char target[MAX_PATH_LEN];
   if (file_path && file_path[0])
   {
      /* Resolve the mutation target: absolute path as-is, else relative to cwd.
       * Keying on the target (not just cwd) catches an absolute Edit into the
       * main clone from a worktree session, and vice-versa. */
      int n;
      if (aimee_path_is_absolute(file_path))
         n = snprintf(target, sizeof(target), "%s", file_path);
      else if (cwd && cwd[0])
         n = snprintf(target, sizeof(target), "%s/%s", cwd, file_path);
      else
         n = snprintf(target, sizeof(target), "%s", file_path);
      if (n >= (int)sizeof(target))
         return 0; /* truncated -> fail-open */
   }
   else if (cwd && cwd[0])
   {
      if (snprintf(target, sizeof(target), "%s", cwd) >= (int)sizeof(target))
         return 0;
   }
   else
      return 0; /* nothing to check -> fail-open */
   return aimee_path_is_main_clone(target);
}

int aimee_main_clone_edits_allowed(const char *repo_cwd)
{
   const char *e = getenv("AIMEE_ALLOW_MAIN_CHECKOUT");
   if (e && (strcmp(e, "1") == 0 || strcmp(e, "true") == 0 || strcmp(e, "yes") == 0))
      return 1;
   char cur[MAX_PATH_LEN];
   snprintf(cur, sizeof(cur), "%s", repo_cwd ? repo_cwd : "");
   for (;;)
   {
      char m[MAX_PATH_LEN + 32];
      snprintf(m, sizeof(m), "%s/.git/aimee-allow-main-edits", cur);
      struct stat st;
      if (cur[0] && stat(m, &st) == 0)
         return 1;
      char *slash = strrchr(cur, '/');
      if (!slash || slash == cur)
         return 0;
      *slash = '\0';
   }
}
