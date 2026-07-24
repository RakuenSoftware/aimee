/* repetition_collapse.c: see repetition_collapse.h for the contract.
 *
 * The module is deliberately dependency-light so the unit test links without
 * pulling in any of the core lib (slop_detect, cJSON, util). All scanning is a
 * single left-to-right pass over the byte buffer with hand-rolled
 * recursive-descent for both markdown structural regions and the JSON
 * grammar that backs the fragment allowlist.
 */
#include "headers/repetition_collapse.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Small byte-stream helpers
 * --------------------------------------------------------------------------- */

static int is_ws(char c)
{
   return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/* Find the byte offset of the first byte of the line containing `pos`. */
/* Find the byte offset one past the next '\n' (or len if none). */
static size_t next_line_end(const char *buf, size_t len, size_t pos)
{
   while (pos < len && buf[pos] != '\n')
      pos++;
   if (pos < len)
      pos++;
   return pos;
}

/* ---------------------------------------------------------------------------
 * rc_range_overlap
 * --------------------------------------------------------------------------- */

int rc_range_overlap(size_t a_start, size_t a_end, size_t b_start, size_t b_end)
{
   if (a_end <= a_start || b_end <= b_start)
      return 0;
   return a_end > b_start && b_end > a_start;
}

/* ---------------------------------------------------------------------------
 * rc_parse_regions
 *
 * One left-to-right scan that classifies every line into at most one region
 * kind (CODE_BLOCK beats LIST_BODY beats TABLE, in that priority), then emits
 * one byte-range region per continuous run.
 * --------------------------------------------------------------------------- */

static int line_is_fence_open(const char *line, size_t llen, int *fence_char_out, size_t *fence_run_out)
{
   size_t i = 0;
   while (i < llen && is_ws(line[i]))
      i++;
   if (i >= llen)
      return 0;
   char ch = line[i];
   if (ch != '`' && ch != '~')
      return 0;
   size_t j = i;
   while (j < llen && line[j] == ch)
      j++;
   size_t run = j - i;
   /* Fences need >= 3 backticks or >= 3 tildes. */
   if (run < 3)
      return 0;
   /* The remainder of the line (past the run, with trailing whitespace
    * tolerated) must be either empty or carry an info string. We accept any
    * non-empty content here - the closing fence must be byte-equal up to run
    * length and must be a fence open on its own line. */
   size_t k = j;
   while (k < llen && is_ws(line[k]))
      k++;
   if (k < llen && line[k] == '`')
      return 0; /* CommonMark: a backtick fence whose info string contains backticks is not a code fence. */
   *fence_char_out = (int)ch;
   *fence_run_out  = run;
   return 1;
}

static int line_is_fence_close(const char *line, size_t llen, int fence_char, size_t fence_run)
{
   size_t i = 0;
   while (i < llen && is_ws(line[i]))
      i++;
   if (i >= llen)
      return 0;
   if (line[i] != (char)fence_char)
      return 0;
   size_t j = i;
   while (j < llen && line[j] == (char)fence_char)
      j++;
   if (j - i < fence_run)
      return 0;
   size_t k = j;
   while (k < llen && is_ws(line[k]))
      k++;
   return k == llen; /* closing fence: trailing ws only */
}

static int line_is_table_row(const char *line, size_t llen)
{
   /* A pipe on the line (ignoring leading whitespace and code-spans) marks it
    * as a table row. The separator line (---|---|---) is also a table row. */
   for (size_t i = 0; i < llen; i++)
   {
      if (line[i] == '`')
      {
         /* skip inline code span: `` ` `` ... `` ` `` */
         size_t j = i;
         while (j < llen && line[j] == '`')
            j++;
         if (j > i)
         {
            size_t run = j - i;
            size_t k   = j;
            while (k < llen)
            {
               if (k + run <= llen && memcmp(line + k, "`", 1) == 0)
               {
                  size_t end_run = 0;
                  while (k + end_run < llen && line[k + end_run] == '`')
                     end_run++;
                  if (end_run >= run)
                  {
                     k += end_run;
                     i  = k;
                     if (i >= llen)
                        goto done;
                     break;
                  }
               }
               k++;
            }
            continue;
         }
      }
      if (line[i] == '|')
         return 1;
   done:
      ;
   }
   return 0;
}

static int line_is_indented_code(const char *line, size_t llen)
{
   size_t i = 0;
   size_t spaces = 0;
   while (i < llen && (line[i] == ' ' || line[i] == '\t'))
   {
      spaces += (line[i] == '\t') ? 4 : 1;
      i++;
   }
   if (i >= llen)
      return 0; /* blank line, not a code line */
   return spaces >= 4;
}

static int line_is_list_marker(const char *line, size_t llen, size_t *marker_end_out)
{
   size_t i = 0;
   while (i < llen && (line[i] == ' ' || line[i] == '\t'))
      i++;
   if (i >= llen)
      return 0;
   char c = line[i];
   /* unordered: -, *, + */
   if (c == '-' || c == '*' || c == '+')
   {
      if (i + 1 < llen && (line[i + 1] == ' ' || line[i + 1] == '\t'))
      {
         *marker_end_out = i + 2;
         return 1;
      }
      return 0;
   }
   /* ordered: 1. / 1) */
   if (c >= '0' && c <= '9')
   {
      size_t j = i;
      while (j < llen && line[j] >= '0' && line[j] <= '9')
         j++;
      if (j < llen && (line[j] == '.' || line[j] == ')') && j + 1 < llen &&
          (line[j + 1] == ' ' || line[j + 1] == '\t'))
      {
         *marker_end_out = j + 2;
         return 1;
      }
   }
   return 0;
}

static void emit_region(rc_region_set_t *out, rc_region_kind_t kind,
                        size_t start, size_t end)
{
   if (start >= end)
      return;
   if (out->count >= RC_MAX_REGIONS)
      return;
   out->regions[out->count].kind  = kind;
   out->regions[out->count].start = start;
   out->regions[out->count].end   = end;
   out->count++;
}

size_t rc_parse_regions(const char *buf, size_t len, rc_region_set_t *out)
{
   if (out)
      out->count = 0;
   if (!buf || len == 0 || !out)
      return 0;

   size_t line_begin = 0;
   int    in_fence   = 0;
   int    fence_char = 0;
   size_t fence_run  = 0;

   size_t run_start          = 0;
   rc_region_kind_t run_kind = RC_REGION_NONE;

#define FLUSH_RUN()                                              \
   do                                                            \
   {                                                             \
      if (run_kind != RC_REGION_NONE)                            \
      {                                                          \
         emit_region(out, run_kind, run_start, line_begin);      \
         run_kind = RC_REGION_NONE;                              \
      }                                                          \
   } while (0)

   while (line_begin < len)
   {
      size_t line_end = next_line_end(buf, len, line_begin);
      const char *line = buf + line_begin;
      size_t llen      = line_end - line_begin;
      /* strip trailing newline for analysis */
      size_t scan_len = llen;
      if (scan_len > 0 && line[scan_len - 1] == '\n')
         scan_len--;
      if (scan_len > 0 && line[scan_len - 1] == '\r')
         scan_len--;

      if (in_fence)
      {
         if (line_is_fence_close(line, scan_len, fence_char, fence_run))
         {
            FLUSH_RUN();
            in_fence = 0;
         }
         else
         {
            if (run_kind == RC_REGION_NONE)
            {
               run_kind = RC_REGION_FENCE;
               run_start = line_begin;
            }
         }
      }
      else
      {
         size_t marker_end = 0;
         int is_list = line_is_list_marker(line, scan_len, &marker_end);
         int is_code = line_is_indented_code(line, scan_len);
         int is_tbl  = line_is_table_row(line, scan_len);
         size_t fence_run_o = 0;

         if (line_is_fence_open(line, scan_len, &fence_char, &fence_run_o))
         {
            FLUSH_RUN();
            in_fence  = 1;
            fence_run = fence_run_o;
         }
         else if (is_code)
         {
            if (run_kind != RC_REGION_CODE_BLOCK)
            {
               FLUSH_RUN();
               run_kind = RC_REGION_CODE_BLOCK;
               run_start = line_begin;
            }
         }
         else if (is_tbl)
         {
            if (run_kind != RC_REGION_TABLE)
            {
               FLUSH_RUN();
               run_kind = RC_REGION_TABLE;
               run_start = line_begin;
            }
         }
         else if (is_list)
         {
            /* LIST_BODY covers the entire list-item line (marker + content)
             * so any verbatim repeat whose byte range straddles a list item
             * - whether or not the period aligns to the marker - is gated.
             * Limitation: indented continuation lines of a multi-line list
             * item are NOT part of the LIST_BODY region. A verbatim repeat
             * that lives entirely in continuation lines will NOT be
             * suppressed; the corpus fixture legit_multiline_list_item
             * locks the documented behavior. */
            FLUSH_RUN();
            size_t line_end_no_nl = line_end;
            if (line_end_no_nl > line_begin && buf[line_end_no_nl - 1] == '\n')
               line_end_no_nl--;
            if (line_end_no_nl > line_begin && buf[line_end_no_nl - 1] == '\r')
               line_end_no_nl--;
            emit_region(out, RC_REGION_LIST_BODY, line_begin, line_end_no_nl);
         }
         else
         {
            FLUSH_RUN();
         }
      }

      line_begin = line_end;
   }
   FLUSH_RUN();
#undef FLUSH_RUN
   return out->count;
}

/* ---------------------------------------------------------------------------
 * rc_parse_json_spans
 *
 * Minimal recursive-descent JSON parser used ONLY to find byte spans of
 * well-formed JSON fragments. Input that is not well-formed produces zero
 * spans - those inputs are heterogeneous objects. Skips over leading
 * whitespace between fragments so multiple fragments parse cleanly.
 * --------------------------------------------------------------------------- */

typedef struct
{
   const char *buf;
   size_t      len;
   size_t      pos;
} jp_t;

/* Forward declarations. */
static int jp_skip_ws(jp_t *jp);
static int jp_parse_value(jp_t *jp, size_t *end_out);

static int jp_skip_ws(jp_t *jp)
{
   while (jp->pos < jp->len && is_ws(jp->buf[jp->pos]))
      jp->pos++;
   return jp->pos < jp->len;
}

/* Match a literal ASCII character at the current position. */
static int jp_match(jp_t *jp, char c)
{
   if (jp->pos < jp->len && jp->buf[jp->pos] == c)
   {
      jp->pos++;
      return 1;
   }
   return 0;
}

/* Match an expected string at the current position. */
static int is_hex(char c)
{
   return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int jp_parse_string(jp_t *jp)
{
   if (!jp_match(jp, '"'))
      return 0;
   while (jp->pos < jp->len)
   {
      unsigned char c = (unsigned char)jp->buf[jp->pos++];
      if (c == '"')
         return 1;
      if (c < 0x20)
         return 0;
      if (c == '\\')
      {
         if (jp->pos >= jp->len)
            return 0;
         char e = jp->buf[jp->pos++];
         if (e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' ||
             e == 'r' || e == 't')
            continue;
         if (e == 'u')
         {
            if (jp->pos + 4 > jp->len)
               return 0;
            for (int i = 0; i < 4; i++)
            {
               if (!is_hex(jp->buf[jp->pos + i]))
                  return 0;
            }
            jp->pos += 4;
            continue;
         }
         return 0;
      }
   }
   return 0;
}

static int jp_parse_number(jp_t *jp)
{
   if (jp->pos >= jp->len)
      return 0;
   if (jp->buf[jp->pos] == '-')
      jp->pos++;
   if (jp->pos >= jp->len)
      return 0;
   if (jp->buf[jp->pos] == '0')
   {
      jp->pos++;
      /* Leading zeroes are not valid JSON numbers. */
      if (jp->pos < jp->len && jp->buf[jp->pos] >= '0' && jp->buf[jp->pos] <= '9')
         return 0;
   }
   else if (jp->buf[jp->pos] >= '1' && jp->buf[jp->pos] <= '9')
   {
      jp->pos++;
      while (jp->pos < jp->len && jp->buf[jp->pos] >= '0' && jp->buf[jp->pos] <= '9')
         jp->pos++;
   }
   else
      return 0;

   if (jp->pos < jp->len && jp->buf[jp->pos] == '.')
   {
      jp->pos++;
      if (jp->pos >= jp->len || jp->buf[jp->pos] < '0' || jp->buf[jp->pos] > '9')
         return 0;
      while (jp->pos < jp->len && jp->buf[jp->pos] >= '0' && jp->buf[jp->pos] <= '9')
         jp->pos++;
   }
   if (jp->pos < jp->len && (jp->buf[jp->pos] == 'e' || jp->buf[jp->pos] == 'E'))
   {
      jp->pos++;
      if (jp->pos < jp->len && (jp->buf[jp->pos] == '+' || jp->buf[jp->pos] == '-'))
         jp->pos++;
      if (jp->pos >= jp->len || jp->buf[jp->pos] < '0' || jp->buf[jp->pos] > '9')
         return 0;
      while (jp->pos < jp->len && jp->buf[jp->pos] >= '0' && jp->buf[jp->pos] <= '9')
         jp->pos++;
   }
   return 1;
}

static int jp_parse_literal(jp_t *jp, const char *kw)
{
   size_t n = strlen(kw);
   if (jp->pos + n > jp->len)
      return 0;
   if (memcmp(jp->buf + jp->pos, kw, n) != 0)
      return 0;
   jp->pos += n;
   return 1;
}

static int jp_parse_array(jp_t *jp, size_t *end_out)
{
   if (!jp_match(jp, '['))
      return 0;
   if (jp_match(jp, ']'))
   {
      *end_out = jp->pos;
      return 1;
   }
   for (;;)
   {
      size_t item_end = 0;
      if (!jp_skip_ws(jp) || !jp_parse_value(jp, &item_end))
         return 0;
      (void)jp_skip_ws(jp);
      if (jp_match(jp, ','))
         continue;
      if (jp_match(jp, ']'))
      {
         *end_out = jp->pos;
         return 1;
      }
      return 0;
   }
}

static int jp_parse_object(jp_t *jp, size_t *end_out)
{
   if (!jp_match(jp, '{'))
      return 0;
   if (jp_match(jp, '}'))
   {
      *end_out = jp->pos;
      return 1;
   }
   for (;;)
   {
      if (!jp_skip_ws(jp) || !jp_parse_string(jp))
         return 0;
      if (!jp_skip_ws(jp) || !jp_match(jp, ':'))
         return 0;
      size_t v_end = 0;
      if (!jp_skip_ws(jp) || !jp_parse_value(jp, &v_end))
         return 0;
      (void)jp_skip_ws(jp);
      if (jp_match(jp, ','))
         continue;
      if (jp_match(jp, '}'))
      {
         *end_out = jp->pos;
         return 1;
      }
      return 0;
   }
}

static int jp_parse_value(jp_t *jp, size_t *end_out)
{
   (void)end_out; /* value end tracked by jp->pos */
   if (!jp_skip_ws(jp))
      return 0;
   char c = jp->buf[jp->pos];
   if (c == '"')
      return jp_parse_string(jp);
   if (c == '{')
      return jp_parse_object(jp, end_out);
   if (c == '[')
      return jp_parse_array(jp, end_out);
   if (c == 't')
      return jp_parse_literal(jp, "true");
   if (c == 'f')
      return jp_parse_literal(jp, "false");
   if (c == 'n')
      return jp_parse_literal(jp, "null");
   if (c == '-' || (c >= '0' && c <= '9'))
      return jp_parse_number(jp);
   return 0;
}

/* Try to parse one JSON value at offset jp->pos. On success, return the
 * exclusive end position via *end_out and advance jp->pos. */
static int jp_try_one(jp_t *jp, size_t *end_out)
{
   size_t save = jp->pos;
   if (!jp_skip_ws(jp))
      return 0;
   /* Require a container opener at the top level - bare scalars (numbers,
    * true/false/null, strings) cannot start a JSON fragment because they
    * collide with too much free-form prose. */
   if (jp->pos >= jp->len)
      return 0;
   char c = jp->buf[jp->pos];
   if (c != '{' && c != '[')
   {
      jp->pos = save;
      return 0;
   }
   size_t value_start = jp->pos;
   size_t value_end   = 0;
   int    ok          = jp_parse_value(jp, &value_end);
   if (ok)
      *end_out = value_end > value_start ? value_end : jp->pos;
   jp->pos = save;
   return ok;
}

size_t rc_parse_json_spans(const char *buf, size_t len, rc_json_span_set_t *out)
{
   if (out)
      out->count = 0;
   if (!buf || len == 0 || !out)
      return 0;
   jp_t jp;
   jp.buf  = buf;
   jp.len  = len;
   jp.pos  = 0;

   while (jp.pos < len)
   {
      size_t end_off = 0;
      size_t old_pos = jp.pos;
      if (jp_try_one(&jp, &end_off))
      {
         if (out->count < RC_MAX_JSON_SPANS)
         {
            out->spans[out->count].start = old_pos;
            out->spans[out->count].end   = end_off;
            out->count++;
         }
         jp.pos = end_off;
         jp_skip_ws(&jp);
         continue;
      }
      /* Heterogeneous / non-grammar byte - skip one byte and keep scanning. */
      jp.pos = old_pos + 1;
   }
   return out->count;
}

/* ---------------------------------------------------------------------------
 * Verbatim periodicity check
 *
 * A "repeat" is a contiguous byte range [s, s+L) that occurs consecutively
 * >=N times in the buffer. We want the LONGEST such trailing repeat (matching
 * the "the loop is decided at one token" thesis in the proposal) but only if
 * it does NOT overlap a suppressed region (structural or JSON-grammar).
 * --------------------------------------------------------------------------- */

static int region_suppresses_range(const rc_region_set_t   *regions,
                                   const rc_json_span_set_t *jsons,
                                   size_t start, size_t end)
{
   if (regions)
   {
      for (size_t i = 0; i < regions->count; i++)
      {
         if (rc_range_overlap(start, end, regions->regions[i].start, regions->regions[i].end))
            return 1;
      }
   }
   if (jsons)
   {
      for (size_t i = 0; i < jsons->count; i++)
      {
         if (rc_range_overlap(start, end, jsons->spans[i].start, jsons->spans[i].end))
            return 1;
      }
   }
   return 0;
}

/* Returns the start of the longest trailing repeat, or 0 if none within
 * `min_span_bytes` exists. The match is greedy: starting from the latest
 * possible position, we widen leftward while the trailing N copies remain
 * byte-equal. */
static size_t find_loop_start(const char *buf, size_t len,
                              size_t min_span, size_t min_repeats,
                              size_t *out_span_bytes, size_t *out_repeats)
{
   if (len < min_repeats * min_span)
      return SIZE_MAX;
   /* Try each candidate period length L from min_span..max. */
   size_t max_period = len / min_repeats;
   /* Walk backwards from the end and find the largest L such that the final
    * N=L*repeats bytes repeat verbatim. For each L, check all N copies. */
   size_t best_start = 0;
   size_t best_len   = 0;
   size_t best_rep   = 0;
   for (size_t L = min_span; L <= max_period; L++)
   {
      size_t total     = L * min_repeats;
      size_t start     = len - total;
      /* Verify each copy is byte-equal to bytes [start, start+L). */
      int bad = 0;
      for (size_t r = 1; r < min_repeats; r++)
      {
         if (memcmp(buf + start, buf + start + r * L, L) != 0)
         {
            bad = 1;
            break;
         }
      }
      if (bad)
         continue;
      /* Count possible extra repeats to the left. */
      size_t repeats = min_repeats;
      while (start >= L && memcmp(buf + start - L, buf + start, L) == 0)
      {
         repeats++;
         start -= L;
      }
      if (L > best_len || (L == best_len && repeats > best_rep))
      {
         best_start = start;
         best_len   = L;
         best_rep   = repeats;
      }
   }
   /* Also check the common case where the input consists of an exact N-period
    * block with no leading non-repeating prefix: try L from min_span up to len. */
   if (best_len == 0)
   {
      /* Already exhausted via the loop above (which considered all L from
       * min_span..max_period with required N copies at the tail). */
   }
   if (best_len == 0)
      return SIZE_MAX;
   *out_span_bytes = best_len;
   *out_repeats    = best_rep;
   return best_start;
}

/* Public detector. The "trailing" check is fine for direct invocation - the
 * caller always passes the most-recent bytes of the stream. If the candidate
 * repeat falls inside a structural region or a JSON grammar span, fall back
 * to earlier repeats of the same period; if all of them are suppressed, the
 * detector abstains (hit=0). */
void rc_detect(const char *buf, size_t len,
               size_t min_span_bytes, size_t min_repeats,
               rc_result_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!buf || !out)
      return;
   if (min_span_bytes == 0)
      min_span_bytes = RC_DEFAULT_MIN_SPAN_BYTES;
   if (min_repeats == 0)
      min_repeats = RC_DEFAULT_MIN_REPEATS;
   if (len < min_span_bytes * min_repeats)
      return;

   rc_region_set_t   regions;
   rc_json_span_set_t jsons;
   (void)rc_parse_regions(buf, len, &regions);
   (void)rc_parse_json_spans(buf, len, &jsons);

   size_t span_bytes = 0;
   size_t repeats    = 0;
   size_t start      = find_loop_start(buf, len, min_span_bytes, min_repeats, &span_bytes, &repeats);
   if (start == SIZE_MAX)
      return;
   /* Walk candidate loop-start positions backwards (each is span_bytes apart)
    * and pick the first one whose repeat range does NOT overlap a suppressed
    * region. */
   size_t candidate = start;
   for (;;)
   {
      size_t cand_end = candidate + repeats * span_bytes;
      if (cand_end > len)
         cand_end = len;
      if (!region_suppresses_range(&regions, &jsons, candidate, cand_end))
      {
         out->hit               = 1;
         out->loop_start_offset = candidate;
         out->loop_span_bytes   = span_bytes;
         out->repeats           = repeats;
         return;
      }
      if (candidate < span_bytes)
         break;
      candidate -= span_bytes;
      if (repeats > 1)
         repeats--;
      else
         break;
   }
   /* All candidates were suppressed - detector abstains. */
}

/* ---------------------------------------------------------------------------
 * rc_metrics - pure helper, exposed for CI
 * --------------------------------------------------------------------------- */

void rc_metrics(int tp, int fp, int tn, int fn,
                double *out_precision,
                double *out_recall,
                double *out_specificity)
{
   double p = (tp + fp > 0) ? (double)tp / (double)(tp + fp) : 1.0;
   double r = (tp + fn > 0) ? (double)tp / (double)(tp + fn) : 1.0;
   double s = (tn + fp > 0) ? (double)tn / (double)(tn + fp) : 1.0;
   if (out_precision)
      *out_precision = p;
   if (out_recall)
      *out_recall = r;
   if (out_specificity)
      *out_specificity = s;
}
