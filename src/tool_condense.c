/* tool_condense.c: deterministic tool-output condensation primitives (Slice 1).
 * See tool_condense.h. Pure string transforms — no I/O, no LLM, no config beyond the
 * enable gate. CORE layer: depends only on config.h + libc. */
#include "tool_condense.h"

#include <stdint.h>
#include <stdio.h> /* snprintf, fopen */
#include <stdlib.h>
#include <string.h>

int tool_condense_enabled(const config_t *cfg)
{
   return cfg && cfg->reduce_command_filter ? 1 : 0;
}

/* ---- command recognition (Slice 2) ---- */

#define TC_MAXTOK 24

/* Tokenize a simple command line into argv (whitespace-separated, single/double quotes
 * honored + stripped). Returns the token count, or -1 if the line contains a shell
 * COMPOUND operator (| ; & newline), a command substitution ($( or backtick), or a
 * glob/redirect we won't reason about — the caller then treats the line as UNRECOGNIZED
 * (fail-open passthrough). Redirect operators end token collection (the command name is
 * before them). Tokens are copied into `store` (a caller buffer of >= TC_MAXTOK*64). */
static int tc_tokenize(const char *s, char *tok[TC_MAXTOK], char *store, size_t storecap)
{
   int n = 0;
   size_t used = 0;
   while (*s)
   {
      while (*s == ' ' || *s == '\t')
         s++;
      if (!*s)
         break;
      /* compound operators / substitution / newline -> bail (compound line) */
      if (*s == '|' || *s == ';' || *s == '&' || *s == '\n' || *s == '`')
         return -1;
      if (*s == '$' && s[1] == '(')
         return -1;
      /* a redirect ends the command portion — argv[0..] already captured */
      if (*s == '<' || *s == '>')
         break;
      if (n >= TC_MAXTOK)
         break; /* enough tokens to recognize the command */
      /* accumulate one token, honoring '…' and "…" quoting */
      char *out = store + used;
      size_t tlen = 0;
      while (*s && *s != ' ' && *s != '\t' && *s != '|' && *s != ';' && *s != '&' && *s != '<' &&
             *s != '>' && *s != '\n')
      {
         char q = 0;
         if (*s == '\'' || *s == '"')
         {
            q = *s++;
            while (*s && *s != q)
            {
               if (used + tlen + 2 >= storecap)
                  return -1;
               out[tlen++] = *s++;
            }
            if (*s == q)
               s++;
            continue;
         }
         if (used + tlen + 2 >= storecap)
            return -1;
         out[tlen++] = *s++;
      }
      out[tlen] = '\0';
      used += tlen + 1;
      tok[n++] = out;
   }
   return n;
}

/* basename of a path token (after the last '/'). */
static const char *tc_base(const char *p)
{
   const char *b = strrchr(p, '/');
   return b ? b + 1 : p;
}

/* A known single-command wrapper that takes the inner command as its trailing args.
 * Returns the number of leading tokens to skip to reach the inner command, or 0 if `t`
 * is not such a wrapper. `next` is the following token (may be NULL) for 2-word forms. */
static int tc_wrapper_skip(const char *t, const char *next)
{
   /* prefix-only wrappers: <wrapper> <inner…> */
   static const char *const one[] = {"time", "nice", "nohup", "stdbuf", "npx", NULL};
   for (int i = 0; one[i]; i++)
      if (strcmp(t, one[i]) == 0)
         return 1;
   /* two-word runners: <a> <b> <inner…> */
   if (next)
   {
      if ((strcmp(t, "uv") == 0 || strcmp(t, "poetry") == 0 || strcmp(t, "pipenv") == 0) &&
          strcmp(next, "run") == 0)
         return 2;
      if (strcmp(t, "bun") == 0 && strcmp(next, "x") == 0)
         return 2;
      if ((strcmp(t, "pnpm") == 0 || strcmp(t, "npm") == 0 || strcmp(t, "yarn") == 0) &&
          strcmp(next, "exec") == 0)
         return 2;
   }
   return 0;
}

/* Is `c` a command whose output we intend to condense (a family lands in a later slice)? */
static int tc_is_recognized_cmd(const char *c)
{
   static const char *const known[] = {
       "git",    "cargo",  "go",    "pytest",  "jest",  "vitest", "mocha",  "ctest", "mvn",
       "gradle", "dotnet", "tsc",   "eslint",  "ruff",  "mypy",   "flake8", "rustc", "gcc",
       "g++",    "cc",     "clang", "clang++", "ls",    "grep",   "rg",     "find",  "npm",
       "yarn",   "pnpm",   "pip",   "pip3",    "cmake", NULL};
   for (int i = 0; known[i]; i++)
      if (strcmp(c, known[i]) == 0)
         return 1;
   return 0;
}

/* Is `t` a leading `VAR=VALUE` environment assignment (identifier before the first '=',
 * not an option, not a path)? */
static int tc_is_var_assign(const char *t)
{
   const char *eq = strchr(t, '=');
   if (!eq || eq == t || t[0] == '-' || strchr(t, '/'))
      return 0;
   for (const char *p = t; p < eq; p++)
      if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '_'))
         return 0;
   return 1;
}

/* A subcommand-bearing command whose 2nd token is meaningful for family routing. */
static int tc_has_subcommand(const char *c)
{
   static const char *const subc[] = {"git",  "cargo", "go",  "dotnet", "npm",
                                      "yarn", "pnpm",  "pip", "pip3",   NULL};
   for (int i = 0; subc[i]; i++)
      if (strcmp(c, subc[i]) == 0)
         return 1;
   return 0;
}

tc_reco_result_t tc_recognize(const char *cmdline)
{
   tc_reco_result_t r;
   r.outcome = TC_UNRECOGNIZED;
   r.cmd[0] = '\0';
   r.sub[0] = '\0';
   if (!cmdline)
      return r;

   char *tok[TC_MAXTOK];
   char store[TC_MAXTOK * 64];
   int n = tc_tokenize(cmdline, tok, store, sizeof store);
   if (n <= 0)
      return r; /* empty or compound -> UNRECOGNIZED (passthrough) */

   int i = 0;
   /* strip a leading VAR=VALUE prefix, `env` + its assignments, sudo, and chained
    * single-command wrappers, until we reach the real inner command. */
   int guard = 0;
   while (i < n && guard++ < TC_MAXTOK)
   {
      /* bare `VAR=VALUE cmd` prefix */
      if (tc_is_var_assign(tok[i]))
      {
         i++;
         continue;
      }
      const char *b = tc_base(tok[i]);
      if (strcmp(b, "env") == 0)
      {
         i++;
         while (i < n && tc_is_var_assign(tok[i]))
            i++;
         continue;
      }
      if (strcmp(b, "sudo") == 0)
      {
         i++;
         while (i < n && tok[i][0] == '-')
         {
            /* sudo short options that take a separate argument (best-effort): consume
             * both the option and its value (e.g. `-u ci`). --opt=val is self-contained. */
            const char *o = tok[i];
            int takes_arg = (o[1] && strchr("ugpCrtTURhDP", o[1]) && o[2] == '\0');
            i++;
            if (takes_arg && i < n)
               i++;
         }
         continue;
      }
      int skip = tc_wrapper_skip(b, (i + 1 < n) ? tok[i + 1] : NULL);
      if (skip)
      {
         i += skip;
         continue;
      }
      break;
   }
   if (i >= n)
      return r;

   const char *cmd = tc_base(tok[i]);
   snprintf(r.cmd, sizeof r.cmd, "%s", cmd);

   /* multiplexers, make, and shell interpreters are always OPAQUE (arbitrary output —
    * only a generic fallback may ever apply, never a family rule). */
   if (strcmp(cmd, "xargs") == 0 || strcmp(cmd, "make") == 0 || strcmp(cmd, "bash") == 0 ||
       strcmp(cmd, "sh") == 0 || strcmp(cmd, "zsh") == 0)
   {
      r.outcome = TC_OPAQUE;
      return r;
   }
   /* ANY path-prefixed invocation is OPAQUE — we only apply a family rule to a BARE
    * command name (resolved by the shell against $PATH). Honoring the basename of a
    * path would let `./git` / `/tmp/git` (a script or binary that merely shares a known
    * name) inherit that command's family filter; treat all such as opaque. */
   if (tok[i][0] == '.' || strchr(tok[i], '/'))
   {
      r.outcome = TC_OPAQUE;
      return r;
   }

   if (tc_is_recognized_cmd(cmd))
   {
      r.outcome = TC_RECOGNIZED;
      if (tc_has_subcommand(cmd) && i + 1 < n)
      {
         /* A subcommand (status/test/run/install/…) is a bare word: skip options ('-…')
          * and option-arguments (paths with '/', or KEY=VALUE with '=') — best-effort;
          * the family rule in a later slice re-parses precisely. */
         for (int j = i + 1; j < n; j++)
            if (tok[j][0] != '-' && !strchr(tok[j], '/') && !strchr(tok[j], '='))
            {
               snprintf(r.sub, sizeof r.sub, "%s", tok[j]);
               break;
            }
      }
      return r;
   }
   return r; /* UNRECOGNIZED */
}

/* ---- a minimal growable string builder (self-contained so the unit test links only
 * this TU + config) ---- */
typedef struct
{
   char *buf;
   size_t len;
   size_t cap;
   int oom; /* sticky: 1 once an allocation failed */
} sb_t;

static void sb_ensure(sb_t *s, size_t extra)
{
   if (s->oom)
      return;
   if (s->len + extra + 1 <= s->cap)
      return;
   size_t ncap = s->cap ? s->cap * 2 : 256;
   while (ncap < s->len + extra + 1)
      ncap *= 2;
   char *nb = realloc(s->buf, ncap);
   if (!nb)
   {
      s->oom = 1;
      return;
   }
   s->buf = nb;
   s->cap = ncap;
}

static void sb_add(sb_t *s, const char *p, size_t n)
{
   sb_ensure(s, n);
   if (s->oom)
      return;
   memcpy(s->buf + s->len, p, n);
   s->len += n;
   s->buf[s->len] = '\0';
}

static void sb_addc(sb_t *s, char c)
{
   sb_add(s, &c, 1);
}

static void sb_adds(sb_t *s, const char *p)
{
   sb_add(s, p, strlen(p));
}

/* Detach the buffer as a C string (or "" for an empty non-OOM result); NULL on OOM. */
static char *sb_finish(sb_t *s)
{
   if (s->oom)
   {
      free(s->buf);
      return NULL;
   }
   if (!s->buf)
      return strdup(""); /* empty input -> empty output, never NULL-as-OOM */
   return s->buf;
}

/* Call `line` on each line of `in` (without its terminator). `has_nl` says whether the
 * source line ended with '\n' (the final line may not). Returns 0, or -1 if the callback
 * signalled OOM (returned non-zero). */
static const char *next_line(const char *p, size_t *out_len, int *has_nl)
{
   const char *nl = strchr(p, '\n');
   if (nl)
   {
      *out_len = (size_t)(nl - p);
      *has_nl = 1;
      return nl + 1;
   }
   *out_len = strlen(p);
   *has_nl = 0;
   return p + *out_len; /* -> the terminating NUL */
}

/* ---- tc_strip_noise ---- */

/* Copy `line` (length n) into `s`, dropping ANSI CSI escapes and resolving CR redraws
 * (keep only the segment after the last '\r'). */
static void emit_clean_line(sb_t *s, const char *line, size_t n)
{
   /* CR redraw: a terminal rewrites the line after each '\r'; the final state is the
    * text after the LAST '\r'. */
   const char *start = line;
   for (size_t i = 0; i < n; i++)
      if (line[i] == '\r')
         start = line + i + 1;
   n -= (size_t)(start - line);

   for (size_t i = 0; i < n;)
   {
      if (start[i] == 0x1b && i + 1 < n && start[i + 1] == '[')
      {
         /* CSI: ESC '[' params (0x20..0x3f) then a final byte (0x40..0x7e). */
         size_t j = i + 2;
         while (j < n && (unsigned char)start[j] >= 0x20 && (unsigned char)start[j] < 0x40)
            j++;
         if (j < n && (unsigned char)start[j] >= 0x40 && (unsigned char)start[j] <= 0x7e)
            j++; /* consume the valid final byte; a malformed/truncated CSI still drops */
         i = j;
         continue;
      }
      sb_addc(s, start[i]);
      i++;
   }
}

char *tc_strip_noise(const char *in)
{
   if (!in)
      return NULL;
   sb_t s = {0};
   const char *p = in;
   int prev_blank = 0;
   int first = 1;
   while (*p || first)
   {
      size_t n;
      int has_nl;
      const char *np = next_line(p, &n, &has_nl);

      /* Build the cleaned line into a scratch builder to test blank-ness. */
      sb_t line = {0};
      emit_clean_line(&line, p, n);
      if (line.oom)
      {
         free(line.buf);
         s.oom = 1;
         break;
      }
      int is_blank = (line.len == 0);

      if (is_blank && prev_blank)
      {
         /* collapse: skip this blank */
         free(line.buf);
      }
      else
      {
         if (!first)
            sb_addc(&s, '\n');
         if (line.buf)
            sb_add(&s, line.buf, line.len);
         free(line.buf);
         prev_blank = is_blank;
      }
      first = 0;
      if (!has_nl)
         break;
      p = np;
   }
   return sb_finish(&s);
}

/* ---- tc_dedup_lines ---- */

char *tc_dedup_lines(const char *in)
{
   if (!in)
      return NULL;
   sb_t s = {0};
   const char *p = in;
   int first = 1;

   /* previous line (owned copy) + its run count */
   char *prev = NULL;
   size_t prev_len = 0;
   long run = 0;

   int done = 0;
   while (!done)
   {
      size_t n;
      int has_nl;
      const char *np = next_line(p, &n, &has_nl);
      int last = !has_nl;

      int same = (prev && n == prev_len && memcmp(p, prev, n) == 0);
      if (same)
      {
         run++;
      }
      else
      {
         /* flush the previous run */
         if (prev)
         {
            if (!first)
               sb_addc(&s, '\n');
            first = 0;
            sb_add(&s, prev, prev_len);
            if (run > 1)
            {
               char tag[32];
               snprintf(tag, sizeof tag, "  (x%ld)", run);
               sb_adds(&s, tag);
            }
            free(prev);
         }
         prev = malloc(n + 1);
         if (!prev)
         {
            s.oom = 1;
            break;
         }
         memcpy(prev, p, n);
         prev[n] = '\0';
         prev_len = n;
         run = 1;
      }
      if (last)
         done = 1;
      else
         p = np;
   }
   /* flush the trailing run */
   if (prev && !s.oom)
   {
      if (!first)
         sb_addc(&s, '\n');
      sb_add(&s, prev, prev_len);
      if (run > 1)
      {
         char tag[32];
         snprintf(tag, sizeof tag, "  (x%ld)", run);
         sb_adds(&s, tag);
      }
   }
   free(prev);
   return sb_finish(&s);
}

/* ---- tc_truncate_with_signal ---- */

char *tc_truncate_with_signal(const char *in, int head, int tail, const char *signal)
{
   if (!in)
      return NULL;
   if (head < 0)
      head = 0;
   if (tail < 0)
      tail = 0;

   /* Index the line offsets. */
   size_t nlines = 0;
   for (const char *p = in;;)
   {
      size_t n;
      int has_nl;
      const char *np = next_line(p, &n, &has_nl);
      nlines++;
      if (!has_nl)
         break;
      p = np;
   }
   if ((size_t)head + (size_t)tail >= nlines)
      return strdup(in); /* already fits */

   /* line pointers + lengths */
   const char **lp = calloc(nlines, sizeof(*lp));
   size_t *ll = calloc(nlines, sizeof(*ll));
   if (!lp || !ll)
   {
      free(lp);
      free(ll);
      return NULL;
   }
   {
      size_t i = 0;
      for (const char *p = in;;)
      {
         size_t n;
         int has_nl;
         const char *np = next_line(p, &n, &has_nl);
         lp[i] = p;
         ll[i] = n;
         i++;
         if (!has_nl)
            break;
         p = np;
      }
   }

   sb_t s = {0};
   int first = 1;
   size_t elided = 0;
   for (size_t i = 0; i < nlines; i++)
   {
      int keep = (i < (size_t)head) || (i >= nlines - (size_t)tail);
      if (!keep && signal && signal[0])
      {
         /* case-sensitive substring within the line */
         char *tmp = malloc(ll[i] + 1);
         if (!tmp)
         {
            s.oom = 1;
            break;
         }
         memcpy(tmp, lp[i], ll[i]);
         tmp[ll[i]] = '\0';
         if (strstr(tmp, signal))
            keep = 1;
         free(tmp);
      }
      if (keep)
      {
         if (elided)
         {
            char mark[48];
            snprintf(mark, sizeof mark, "... %zu lines elided ...", elided);
            if (!first)
               sb_addc(&s, '\n');
            first = 0;
            sb_adds(&s, mark);
            elided = 0;
         }
         if (!first)
            sb_addc(&s, '\n');
         first = 0;
         sb_add(&s, lp[i], ll[i]);
      }
      else
      {
         elided++;
      }
   }
   if (elided && !s.oom)
   {
      char mark[48];
      snprintf(mark, sizeof mark, "... %zu lines elided ...", elided);
      if (!first)
         sb_addc(&s, '\n');
      sb_adds(&s, mark);
   }
   free(lp);
   free(ll);
   return sb_finish(&s);
}

/* ---- test-runner family (Slice 3) ---- */

/* case-insensitive substring test. */
static int ci_contains(const char *hay, size_t haylen, const char *needle)
{
   size_t nl = strlen(needle);
   if (nl == 0 || nl > haylen)
      return 0;
   for (size_t i = 0; i + nl <= haylen; i++)
   {
      size_t j = 0;
      for (; j < nl; j++)
      {
         char a = hay[i + j], b = needle[j];
         if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
         if (b >= 'A' && b <= 'Z')
            b = (char)(b - 'A' + 'a');
         if (a != b)
            break;
      }
      if (j == nl)
         return 1;
   }
   return 0;
}

static int line_has_any(const char *line, size_t n, const char *const *sigs)
{
   for (int i = 0; sigs[i]; i++)
      if (ci_contains(line, n, sigs[i]))
         return 1;
   return 0;
}

/* signals that mark a FAILURE (never elide these); used for the exit-code safety rule. */
static const char *const TC_FAIL_SIGS[] = {
    "fail", "error", "panic", "assert", "traceback", "exception", "not ok", "✗", "✖", "✘", NULL};
/* additional signals worth keeping (the summary / counts / warnings). */
static const char *const TC_KEEP_SIGS[] = {"test result", "result:", "====",     "collected",
                                           "summary",     "warning", "warnings", NULL};

char *tc_family_test_runner(int exit_code, const char *in)
{
   if (!in)
      return NULL;
   /* index lines */
   size_t nlines = 0;
   for (const char *p = in;;)
   {
      size_t n;
      int has_nl;
      const char *np = next_line(p, &n, &has_nl);
      nlines++;
      if (!has_nl)
         break;
      p = np;
   }
   const char **lp = calloc(nlines, sizeof(*lp));
   size_t *ll = calloc(nlines, sizeof(*ll));
   if (!lp || !ll)
   {
      free(lp);
      free(ll);
      return NULL;
   }
   size_t idx = 0;
   int any_fail = 0;
   for (const char *p = in;;)
   {
      size_t n;
      int has_nl;
      const char *np = next_line(p, &n, &has_nl);
      lp[idx] = p;
      ll[idx] = n;
      if (line_has_any(p, n, TC_FAIL_SIGS))
         any_fail = 1;
      idx++;
      if (!has_nl)
         break;
      p = np;
   }
   /* Safety: a non-zero exit with no recognizable failure line -> passthrough (never
    * risk hiding the cause behind a passing-transcript filter). */
   if (exit_code != 0 && !any_fail)
   {
      free(lp);
      free(ll);
      return NULL;
   }

   const size_t HEAD = 2, TAIL = 6;
   sb_t s = {0};
   int first = 1;
   size_t elided = 0;
   for (size_t i = 0; i < nlines; i++)
   {
      int keep = (i < HEAD) || (i + TAIL >= nlines) || line_has_any(lp[i], ll[i], TC_FAIL_SIGS) ||
                 line_has_any(lp[i], ll[i], TC_KEEP_SIGS);
      if (keep)
      {
         if (elided)
         {
            char mark[48];
            snprintf(mark, sizeof mark, "... %zu lines elided ...", elided);
            if (!first)
               sb_addc(&s, '\n');
            first = 0;
            sb_adds(&s, mark);
            elided = 0;
         }
         if (!first)
            sb_addc(&s, '\n');
         first = 0;
         sb_add(&s, lp[i], ll[i]);
      }
      else
      {
         elided++;
      }
   }
   if (elided && !s.oom)
   {
      char mark[48];
      snprintf(mark, sizeof mark, "... %zu lines elided ...", elided);
      if (!first)
         sb_addc(&s, '\n');
      sb_adds(&s, mark);
   }
   free(lp);
   free(ll);
   return sb_finish(&s);
}

/* ---- spill store + top-level apply (Slice 3) ---- */

/* An opaque, deterministic spill ref = FNV-1a-64(cmdline || '\0' || raw), hex. Not a
 * monotonic counter (not enumerable); deterministic so identical output dedups. */
static void tc_hash_ref(const char *seed, const char *content, char out[40])
{
   uint64_t h = 1469598103934665603ULL;
   for (const char *p = seed; p && *p; p++)
   {
      h ^= (unsigned char)*p;
      h *= 1099511628211ULL;
   }
   h ^= 0;
   h *= 1099511628211ULL;
   for (const char *p = content; p && *p; p++)
   {
      h ^= (unsigned char)*p;
      h *= 1099511628211ULL;
   }
   snprintf(out, 40, "tc-%016llx", (unsigned long long)h);
}

/* Write the full raw output to <dir>/<ref>.out. Returns 0 on a fully-durable write,
 * -1 otherwise (the caller then passes through — never a condense without a backstop). */
static int tc_spill_write(const char *dir, const char *ref, const char *content)
{
   char path[1400];
   if (snprintf(path, sizeof path, "%s/%s.out", dir, ref) >= (int)sizeof path)
      return -1;
   FILE *f = fopen(path, "wb");
   if (!f)
      return -1;
   size_t n = strlen(content);
   size_t w = fwrite(content, 1, n, f);
   /* fflush+fclose success + full write == durable enough for the backstop. */
   return (fclose(f) == 0 && w == n) ? 0 : -1;
}

/* Does the recognized command denote a TEST-RUNNER invocation (the only S3 family)? */
static int tc_is_test_invocation(const tc_reco_result_t *r)
{
   static const char *const runners[] = {"pytest", "jest", "vitest", "mocha", "ctest", NULL};
   for (int i = 0; runners[i]; i++)
      if (strcmp(r->cmd, runners[i]) == 0)
         return 1;
   static const char *const sub_runners[] = {"cargo", "go",     "npm",    "yarn", "pnpm",
                                             "mvn",   "gradle", "dotnet", NULL};
   for (int i = 0; sub_runners[i]; i++)
      if (strcmp(r->cmd, sub_runners[i]) == 0 && strcmp(r->sub, "test") == 0)
         return 1;
   return 0;
}

char *tool_condense_apply(const config_t *cfg, const char *cmdline, int exit_code, const char *raw,
                          const char *spill_dir, tc_stats_t *stats)
{
   if (stats)
   {
      memset(stats, 0, sizeof *stats);
      stats->raw_bytes = raw ? (long)strlen(raw) : 0;
      stats->final_bytes = stats->raw_bytes;
   }
   if (!tool_condense_enabled(cfg) || !raw || !raw[0])
      return NULL;
   long rawlen = (long)strlen(raw);
   if (rawlen > (1 << 20))
      return NULL; /* over the input cap -> hand back to the size-based fallback */

   tc_reco_result_t reco = tc_recognize(cmdline);
   if (stats)
      stats->recognized = (reco.outcome == TC_RECOGNIZED);
   if (reco.outcome != TC_RECOGNIZED)
      return NULL; /* OPAQUE / UNRECOGNIZED -> passthrough (S3 acts only on a family) */

   char *cond = NULL;
   const char *family = "";
   if (tc_is_test_invocation(&reco))
   {
      cond = tc_family_test_runner(exit_code, raw);
      family = "test";
   }
   if (!cond)
      return NULL; /* no family match or the filter declined -> passthrough */

   long condlen = (long)strlen(cond);
   /* material-gain gate: require a real shrink, else it's not worth a spill round-trip. */
   if (rawlen - condlen < 200 || condlen * 100 > rawlen * 85)
   {
      free(cond);
      return NULL;
   }
   /* Lossless-on-demand: the condensed body only ships if the FULL raw is spilled. No
    * spill dir, or a failed spill -> pass through (never a condensed body with no
    * recoverable backstop). */
   if (!spill_dir || !spill_dir[0])
   {
      free(cond);
      return NULL;
   }
   char ref[40];
   tc_hash_ref(cmdline ? cmdline : "", raw, ref);
   if (tc_spill_write(spill_dir, ref, raw) != 0)
   {
      free(cond);
      return NULL;
   }

   sb_t out = {0};
   sb_adds(&out, cond);
   char ptr[96];
   snprintf(ptr, sizeof ptr, "\n... full output (%ld bytes): tool_output_get %s", rawlen, ref);
   sb_adds(&out, ptr);
   free(cond);
   char *final = sb_finish(&out);
   if (stats && final)
   {
      stats->final_bytes = (long)strlen(final);
      stats->spilled = 1;
      snprintf(stats->family, sizeof stats->family, "%s", family);
      snprintf(stats->spill_ref, sizeof stats->spill_ref, "%s", ref);
   }
   return final;
}
