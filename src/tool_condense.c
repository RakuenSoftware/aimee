/* tool_condense.c: deterministic tool-output condensation primitives (Slice 1).
 * See tool_condense.h. Pure string transforms — no I/O, no LLM, no config beyond the
 * enable gate. CORE layer: depends only on config.h + libc. */
#include "tool_condense.h"

#include <stdio.h> /* snprintf */
#include <stdlib.h>
#include <string.h>

int tool_condense_enabled(const config_t *cfg)
{
   return cfg && cfg->reduce_command_filter ? 1 : 0;
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
