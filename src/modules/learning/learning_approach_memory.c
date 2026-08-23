/* learning_approach_memory.c: approach-level negative knowledge (S3).
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */

#include <aimee/learning/approach_memory.h>

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
