/* learning_approach_memory.c: approach-level negative knowledge (S3).
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */

#include <aimee/learning/approach_memory.h>

#include "modules/db2/c/approach_failures.h"

#include <aimee/learning/policy_arms.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Tokens shorter than this carry no topic information and inflate overlap
 * between unrelated goals ("a", "to", "of"). Three, not four: "TLS", "git",
 * "npm", and "SQL" are exactly the specific words a goal turns on. */
#define APPROACH_MIN_TOKEN_LEN 3

/* Function words that survive the length floor and would otherwise inflate
 * every comparison, since almost any two goals share them.
 *
 * The memory module has a curated list of these already
 * (memory_is_stopword_token), but declares it in a PRIVATE header — reaching
 * for it here would cross a module boundary to save duplicating a few strings.
 * This is that list narrowed to the entries the length floor does not already
 * remove. If the two ever need to agree, the fix is to promote memory's to a
 * public contract, not to widen this one.
 *
 * `token` is not NUL-terminated at the call site, so the length is passed. */
static int approach_is_stopword(const char *token, size_t len)
{
   static const char *const words[] = {
       "and",   "are",  "also", "but",   "did",  "for",  "from", "had",  "has",  "have",
       "her",   "him",  "his",  "its",   "just", "like", "not",  "our",  "than", "that",
       "the",   "them", "then", "there", "they", "this", "was",  "were", "what", "when",
       "where", "who",  "why",  "how",   "with", "yes",  "your", NULL};
   for (int i = 0; words[i]; i++)
      if (strlen(words[i]) == len && strncmp(token, words[i], len) == 0)
         return 1;
   return 0;
}

/* Walk `tokens` (space-separated) one token at a time. Returns the token
 * start, sets *len, and advances *cursor; returns NULL at the end. */
static const char *token_next(const char **cursor, size_t *len)
{
   const char *p = *cursor;
   while (*p == ' ')
      p++;
   if (!*p)
      return NULL;
   const char *start = p;
   while (*p && *p != ' ')
      p++;
   *len = (size_t)(p - start);
   *cursor = p;
   return start;
}

static int token_set_has(const char *tokens, const char *needle, size_t needle_len)
{
   const char *cursor = tokens;
   size_t len = 0;
   for (const char *t = token_next(&cursor, &len); t; t = token_next(&cursor, &len))
      if (len == needle_len && strncmp(t, needle, len) == 0)
         return 1;
   return 0;
}

void learning_approach_tokens(const char *text, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (!text || !text[0])
      return;

   size_t o = 0;
   char token[64];
   size_t t = 0;
   for (const char *p = text;; p++)
   {
      unsigned char c = (unsigned char)*p;
      if (isalnum(c))
      {
         if (t + 1 < sizeof(token))
            token[t++] = (char)tolower(c);
         if (*p)
            continue;
      }
      if (t >= APPROACH_MIN_TOKEN_LEN && !approach_is_stopword(token, t))
      {
         token[t] = '\0';
         /* Drop duplicates so a repeated word cannot dominate the overlap. */
         if (!token_set_has(out, token, t) && o + t + 2 < out_len)
         {
            if (o > 0)
               out[o++] = ' ';
            memcpy(out + o, token, t);
            o += t;
            out[o] = '\0';
         }
      }
      t = 0;
      if (!*p)
         break;
   }
}

/* FNV-1a over the normalised token set, in two seeded passes so the signature
 * fills 32 hex characters. */
static uint64_t fnv1a(uint64_t basis, const char *s)
{
   uint64_t h = basis;
   for (const char *p = s; p && *p; p++)
      h = (h ^ (unsigned char)*p) * 1099511628211ULL;
   return h;
}

int learning_approach_signature(const char *text, char *out, size_t out_len)
{
   if (!out || out_len < APPROACH_MEM_SIGNATURE_LEN)
      return -1;
   char tokens[APPROACH_MEM_TOKENS_LEN];
   learning_approach_tokens(text, tokens, sizeof(tokens));
   uint64_t a = fnv1a(1469598103934665603ULL, tokens);
   uint64_t b = fnv1a(0x9e3779b97f4a7c15ULL, tokens);
   snprintf(out, out_len, "%016llx%016llx", (unsigned long long)a, (unsigned long long)b);
   return 0;
}

double learning_approach_overlap(const char *tokens_a, const char *tokens_b)
{
   if (!tokens_a || !tokens_b || !tokens_a[0] || !tokens_b[0])
      return 0.0;

   int in_a = 0, in_both = 0;
   const char *cursor = tokens_a;
   size_t len = 0;
   for (const char *t = token_next(&cursor, &len); t; t = token_next(&cursor, &len))
   {
      in_a++;
      if (token_set_has(tokens_b, t, len))
         in_both++;
   }
   int in_b = 0;
   cursor = tokens_b;
   for (const char *t = token_next(&cursor, &len); t; t = token_next(&cursor, &len))
      in_b++;

   int uni = in_a + in_b - in_both;
   if (uni <= 0)
      return 0.0;
   return (double)in_both / (double)uni;
}

int learning_approach_record_failure(const char *goal, const char *approach,
                                     const char *failure_mode, const char *source,
                                     const char *source_ref)
{
   if (!goal || !goal[0] || !approach || !approach[0])
      return -1;
#if defined(AIMEE_DB2_DISABLED)
   (void)failure_mode;
   (void)source;
   (void)source_ref;
   return -1;
#else
   char goal_sig[APPROACH_MEM_SIGNATURE_LEN];
   char approach_sig[APPROACH_MEM_SIGNATURE_LEN];
   char tokens[APPROACH_MEM_TOKENS_LEN];
   if (learning_approach_signature(goal, goal_sig, sizeof(goal_sig)) != 0 ||
       learning_approach_signature(approach, approach_sig, sizeof(approach_sig)) != 0)
      return -1;
   learning_approach_tokens(goal, tokens, sizeof(tokens));
   if (!tokens[0])
      return -1; /* a goal with no topic words cannot be recalled against */

   return db2_approach_failure_record(goal_sig, goal, tokens, approach_sig, approach, failure_mode,
                                      source, source_ref);
#endif
}

#if !defined(AIMEE_DB2_DISABLED)
/* The rarest-looking token of a goal, used only to narrow the candidate pool
 * before scoring. Picking the LONGEST token is a cheap proxy for specificity;
 * getting it wrong costs recall breadth, never correctness, because the real
 * filter is the overlap score. */
static void narrowing_token(const char *tokens, char *out, size_t out_len)
{
   out[0] = '\0';
   const char *cursor = tokens;
   size_t len = 0, best = 0;
   for (const char *t = token_next(&cursor, &len); t; t = token_next(&cursor, &len))
      if (len > best && len + 1 < out_len)
      {
         best = len;
         memcpy(out, t, len);
         out[len] = '\0';
      }
}
#endif

int learning_approach_recall(const char *goal, learning_approach_hit_t *out, int max)
{
   if (!goal || !goal[0] || !out || max <= 0)
      return -1;
#if defined(AIMEE_DB2_DISABLED)
   return -1;
#else
   char tokens[APPROACH_MEM_TOKENS_LEN];
   learning_approach_tokens(goal, tokens, sizeof(tokens));
   if (!tokens[0])
      return 0;

   char narrow[64];
   narrowing_token(tokens, narrow, sizeof(narrow));

   approach_failure_t rows[64];
   int n = db2_approach_failure_candidates(narrow, rows, (int)(sizeof(rows) / sizeof(rows[0])));
   if (n < 0)
      return -1;

   /* Score, filter, then insertion-sort the survivors by similarity. The pool
    * is bounded and small, so a simple sort beats a heap here. */
   int kept = 0;
   for (int i = 0; i < n; i++)
   {
      double sim = learning_approach_overlap(tokens, rows[i].goal_tokens);
      if (sim < APPROACH_MEM_MIN_SIMILARITY)
         continue;
      learning_approach_hit_t hit;
      memset(&hit, 0, sizeof(hit));
      snprintf(hit.goal_text, sizeof(hit.goal_text), "%s", rows[i].goal_text);
      snprintf(hit.approach_text, sizeof(hit.approach_text), "%s", rows[i].approach_text);
      snprintf(hit.failure_mode, sizeof(hit.failure_mode), "%s", rows[i].failure_mode);
      snprintf(hit.source_ref, sizeof(hit.source_ref), "%s", rows[i].source_ref);
      hit.occurrences = (long long)rows[i].occurrences;
      hit.similarity = sim;

      int pos = kept < max ? kept : max - 1;
      if (kept >= max && out[max - 1].similarity >= sim)
         continue;
      while (pos > 0 && out[pos - 1].similarity < sim)
      {
         out[pos] = out[pos - 1];
         pos--;
      }
      out[pos] = hit;
      if (kept < max)
         kept++;
   }
   return kept;
#endif
}

int learning_approach_render(const char *goal, char *out, size_t out_len, char *arm_out,
                             size_t arm_out_len)
{
   if (arm_out && arm_out_len)
      arm_out[0] = '\0';
   if (!out || out_len == 0)
      return 0;
   out[0] = '\0';

   /* Whether this block is worth its tokens is a question, not a constant. */
   char arm[LEARNING_POLICY_ARM_LEN] = LEARNING_POLICY_ADVISORY_FULL;
   (void)learning_policy_select(LEARNING_POLICY_PLAN_ADVISORY, arm, sizeof(arm));
   if (arm_out && arm_out_len)
      snprintf(arm_out, arm_out_len, "%s", arm);

   if (strcmp(arm, LEARNING_POLICY_ADVISORY_OFF) == 0)
      return 0; /* the control arm: say nothing, so the block can be measured */

   learning_approach_hit_t hits[APPROACH_MEM_MAX_RECALL];
   int n = learning_approach_recall(goal, hits, APPROACH_MEM_MAX_RECALL);
   if (n <= 0)
      return 0;

   if (strcmp(arm, LEARNING_POLICY_ADVISORY_BRIEF) == 0)
   {
      snprintf(out, out_len, "%d approach%s to a goal like this one has already failed.", n,
               n == 1 ? "" : "es");
      return n;
   }

   /* Reports, does not instruct: the planner is told what was tried and what
    * happened, and decides for itself. An imperative here would turn advisory
    * recall into an unreviewed rule. */
   size_t o = (size_t)snprintf(out, out_len,
                               "Approaches already tried for a goal like this, and how they went:");
   for (int i = 0; i < n && o + 1 < out_len; i++)
      o += (size_t)snprintf(out + o, out_len - o, "\n- %s -> %s (seen %lld time%s)",
                            hits[i].approach_text[0] ? hits[i].approach_text : "(unrecorded)",
                            hits[i].failure_mode[0] ? hits[i].failure_mode : "failed",
                            hits[i].occurrences, hits[i].occurrences == 1 ? "" : "s");
   return n;
}
