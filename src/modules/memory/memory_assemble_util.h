/* memory_assemble_util.h: pure, dependency-free helpers extracted from
 * memory_assemble.c so the context-rendering string logic can be unit-tested
 * in isolation. static inline → no link dependency (the jo_type_name /
 * session_start_util pattern); the companion test links nothing extra. */
#ifndef DEC_MEMORY_ASSEMBLE_UTIL_H
#define DEC_MEMORY_ASSEMBLE_UTIL_H 1

#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

/* XML-escape `src` into `dst` (NUL-terminated, never overflowing dst_len),
 * replacing & < > " with their entities. Stops cleanly at the buffer edge
 * (never emits a partial entity). Tolerates NULL/empty src → dst becomes "". */
static inline void xml_escape_text(const char *src, char *dst, size_t dst_len)
{
   if (!dst || dst_len == 0)
      return;
   dst[0] = '\0';
   if (!src || !src[0])
      return;

   size_t used = 0;
   for (const unsigned char *p = (const unsigned char *)src; *p && used + 1 < dst_len; p++)
   {
      const char *rep = NULL;
      switch (*p)
      {
      case '&':
         rep = "&amp;";
         break;
      case '<':
         rep = "&lt;";
         break;
      case '>':
         rep = "&gt;";
         break;
      case '"':
         rep = "&quot;";
         break;
      default:
         break;
      }
      if (rep)
      {
         size_t rlen = strlen(rep);
         if (used + rlen >= dst_len)
            break;
         memcpy(dst + used, rep, rlen);
         used += rlen;
      }
      else
         dst[used++] = (char)*p;
   }
   dst[used] = '\0';
}

/* Map a context section header to the XML tag wrapping its items. Unknown or
 * NULL headers fall back to the generic "memory_item". */
static inline const char *context_xml_tag_for_header(const char *header)
{
   if (!header)
      return "memory_item";
   if (strcmp(header, "Key Facts") == 0)
      return "historical_fact";
   if (strcmp(header, "Mental Models") == 0)
      return "mental_model";
   if (strcmp(header, "Constraints") == 0)
      return "constraint";
   if (strcmp(header, "Procedures") == 0)
      return "procedure_memory";
   if (strcmp(header, "Active Tasks") == 0)
      return "active_task";
   if (strcmp(header, "Recent Context") == 0)
      return "recent_event";
   return "memory_item";
}

/* Lowercase alphanumeric tokens of >=3 chars, hashed into a 64-bit set. Short
 * tokens are dropped because "the"/"is"/"a" appear in everything and would drag
 * every pair toward looking similar. */
static inline unsigned long long assemble_token_bits(const char *s)
{
   unsigned long long bits = 0;
   const unsigned char *p = (const unsigned char *)(s ? s : "");
   while (*p)
   {
      while (*p && !isalnum(*p))
         p++;
      unsigned long long h = 1469598103934665603ULL;
      int n = 0;
      while (*p && isalnum(*p))
      {
         h ^= (unsigned long long)tolower(*p++);
         h *= 1099511628211ULL;
         n++;
      }
      if (n >= 3)
         bits |= 1ULL << (h & 63U);
   }
   return bits;
}

/* Do two texts say the same thing? Jaccard overlap of their token sets at a high
 * threshold.
 *
 * The threshold IS the design decision. Set it low and distinct facts that share
 * vocabulary get suppressed -- silently losing evidence, which is far worse than
 * showing one redundant line. At 0.85 only genuine restatements collide.
 *
 * Lexical, not embedding-based, deliberately: this runs on the READ path, where
 * an embedder round-trip per candidate pair would add latency to every turn.
 *
 * A 64-bit set saturates on very long text, so near-full sets are refused: once
 * most bits are set, everything looks similar to everything. */
static inline int assemble_texts_near_duplicate(const char *a, const char *b)
{
   unsigned long long ba = assemble_token_bits(a);
   unsigned long long bb = assemble_token_bits(b);
   if (!ba || !bb)
      return 0;
   if (__builtin_popcountll(ba) >= 56 || __builtin_popcountll(bb) >= 56)
      return 0;
   unsigned inter = (unsigned)__builtin_popcountll(ba & bb);
   unsigned uni = (unsigned)__builtin_popcountll(ba | bb);
   if (uni == 0)
      return 0;
   /* >= 0.85 without floating point: 100*inter >= 85*uni. */
   return (inter * 100U) >= (uni * 85U);
}

/* Recency factor in (0, 1] for a memory of the given age: a flat plateau
 * followed by a slow half-life, rather than a plain exponential from day zero.
 *
 * The plateau exists because a fact does not become less true as it ages. A
 * correct answer learned three months ago should not lose to a less relevant
 * one learned yesterday, so inside the plateau candidates compete on relevance
 * alone. Past it the tail is long enough that a still-valid old fact stays
 * reachable while genuinely stale material sinks.
 *
 * A negative age means "no usable stamp" and returns 1.0: an unknown age is not
 * evidence of staleness, so it must not be penalised. */
#define MEMORY_RECENCY_PLATEAU_DAYS  180.0
#define MEMORY_RECENCY_HALFLIFE_DAYS 1825.0

static inline double context_recency_from_age_days(double days)
{
   if (days < 0.0 || days <= MEMORY_RECENCY_PLATEAU_DAYS)
      return 1.0;
   return pow(0.5, (days - MEMORY_RECENCY_PLATEAU_DAYS) / MEMORY_RECENCY_HALFLIFE_DAYS);
}

/* Fold `recency` into `score` under the retrieval plan's recency_weight, which
 * runs 0.0 (recency is irrelevant to this intent — score unchanged) through 1.0
 * (score scales by the full decay). Weights outside [0,1] are clamped. */
static inline double context_apply_recency(double score, double recency, double recency_weight)
{
   double w = recency_weight;
   if (w <= 0.0)
      return score;
   if (w > 1.0)
      w = 1.0;
   return score * ((1.0 - w) + w * recency);
}

#endif /* DEC_MEMORY_ASSEMBLE_UTIL_H */
