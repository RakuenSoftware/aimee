/* text.c: text similarity, stemming, tokenization, and search utilities */
#include "aimee.h"
#include <ctype.h>

/* --- Stopwords for search tokenization --- */

static const char *stopwords[] = {
    "the",   "a",      "an",     "and",     "or",    "but",   "in",      "on",   "at",   "to",
    "for",   "of",     "with",   "by",      "from",  "is",    "are",     "was",  "were", "be",
    "been",  "being",  "have",   "has",     "had",   "do",    "does",    "did",  "will", "would",
    "could", "should", "may",    "might",   "can",   "shall", "not",     "no",   "nor",  "so",
    "if",    "then",   "than",   "too",     "very",  "just",  "about",   "up",   "out",  "into",
    "over",  "after",  "before", "between", "under", "again", "further", "once", "here", "there",
    "when",  "where",  "why",    "how",     "all",   "each",  "every",   "both", "few",  "more",
    "most",  "other",  "some",   "such",    "only",  "own",   "same",    "that", "this", "these",
    "those", "it",     "its",    NULL};

static int is_stopword(const char *word)
{
   for (int i = 0; stopwords[i]; i++)
   {
      if (strcmp(word, stopwords[i]) == 0)
         return 1;
   }
   return 0;
}

/* --- Trigram similarity --- */

typedef struct
{
   char tri[4];
} trigram_t;

/* Split a reasoning preamble off the front of a model response. See util.h.
 *
 * Deliberately prefix-anchored and non-destructive: the two things every
 * hand-rolled copy of this got wrong. */
const char *text_split_reasoning_prefix(const char *text, const char **reasoning,
                                        size_t *reasoning_len)
{
   if (reasoning)
      *reasoning = NULL;
   if (reasoning_len)
      *reasoning_len = 0;
   if (!text)
      return NULL;

   const char *p = text;
   while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')
      p++;

   /* A LEADING close tag with no opener. Real providers emit this: the reasoning
    * itself went out-of-band in reasoning_content and only the closing delimiter
    * leaked into content, so there is nothing to hand back as reasoning — the tag
    * is a leftover delimiter, not content. Still prefix-anchored: a close tag
    * anywhere but the front is ordinary text and is left alone. */
   if (strncmp(p, "</think>", 8) == 0)
   {
      const char *answer = p + 8;
      while (*answer == '\n' || *answer == '\r' || *answer == ' ' || *answer == '\t')
         answer++;
      return answer;
   }

   if (strncmp(p, "<think>", 7) != 0)
      return text; /* no preamble: an inner tag is content, leave it alone */

   const char *open = p + 7;
   const char *close = strstr(open, "</think>");
   if (!close)
      return text; /* unterminated: don't guess, hand back the whole thing */

   if (reasoning)
      *reasoning = open;
   if (reasoning_len)
      *reasoning_len = (size_t)(close - open);

   const char *answer = close + 8;
   while (*answer == '\n' || *answer == '\r' || *answer == ' ' || *answer == '\t')
      answer++;
   return answer;
}

/* Drop a trailing partial UTF-8 character. See util.h for why this matters.
 *
 * Walk back from the end over continuation bytes (10xxxxxx) to the character's
 * lead byte, then compare the bytes actually present against the length the lead
 * byte declares. Short => the character was cut by a byte-wise truncation, so
 * terminate at the lead byte and drop it. A well-formed tail is left alone. */
void text_trim_partial_utf8(char *s)
{
   if (!s || !s[0])
      return;

   size_t len = strlen(s);
   /* A UTF-8 character is at most 4 bytes, so the lead byte of the final
    * character is within 4 bytes of the end; anything further back is already a
    * complete character. */
   for (size_t back = 1; back <= 4 && back <= len; back++)
   {
      size_t i = len - back;
      unsigned char c = (unsigned char)s[i];
      if ((c & 0xC0) == 0x80)
         continue; /* continuation byte — keep walking back to the lead */

      size_t need = (c & 0x80) == 0x00   ? 1  /* 0xxxxxxx: ASCII */
                    : (c & 0xE0) == 0xC0 ? 2  /* 110xxxxx */
                    : (c & 0xF0) == 0xE0 ? 3  /* 1110xxxx */
                    : (c & 0xF8) == 0xF0 ? 4  /* 11110xxx */
                                         : 0; /* 10xxxxxx handled above; else invalid */
      if (need == 0 || back < need)
         s[i] = '\0'; /* invalid lead, or too few bytes present: drop the char */
      return;         /* the last character is complete — nothing to do */
   }
   /* Four continuation bytes with no lead: not UTF-8 at all. Leave it; this
    * function's contract is to undo truncation, not to sanitise arbitrary bytes. */
}

size_t text_sanitize_utf8(char *s)
{
   if (!s)
      return 0;

   size_t replaced = 0;
   for (size_t i = 0; s[i];)
   {
      unsigned char c = (unsigned char)s[i];
      size_t need = 0;
      if (c <= 0x7f)
         need = 1;
      else if (c >= 0xc2 && c <= 0xdf)
         need = 2;
      else if (c >= 0xe0 && c <= 0xef)
         need = 3;
      else if (c >= 0xf0 && c <= 0xf4)
         need = 4;

      int valid = need != 0;
      for (size_t j = 1; valid && j < need; j++)
         valid = s[i + j] && ((unsigned char)s[i + j] & 0xc0) == 0x80;

      /* Exclude overlong forms, UTF-16 surrogates, and values above U+10FFFF. */
      if (valid && need == 3)
      {
         unsigned char second = (unsigned char)s[i + 1];
         if ((c == 0xe0 && second < 0xa0) || (c == 0xed && second > 0x9f))
            valid = 0;
      }
      if (valid && need == 4)
      {
         unsigned char second = (unsigned char)s[i + 1];
         if ((c == 0xf0 && second < 0x90) || (c == 0xf4 && second > 0x8f))
            valid = 0;
      }

      if (valid)
      {
         i += need;
         continue;
      }
      s[i++] = '?';
      replaced++;
   }
   return replaced;
}

int text_is_valid_utf8(const char *s)
{
   if (!s)
      return 1;

   for (size_t i = 0; s[i];)
   {
      unsigned char c = (unsigned char)s[i];
      size_t need = 0;
      if (c <= 0x7f)
         need = 1;
      else if (c >= 0xc2 && c <= 0xdf)
         need = 2;
      else if (c >= 0xe0 && c <= 0xef)
         need = 3;
      else if (c >= 0xf0 && c <= 0xf4)
         need = 4;

      if (need == 0)
         return 0;
      for (size_t j = 1; j < need; j++)
         if (!s[i + j] || ((unsigned char)s[i + j] & 0xc0) != 0x80)
            return 0;

      /* Exclude overlong forms, UTF-16 surrogates, and values above U+10FFFF. */
      if (need == 3)
      {
         unsigned char second = (unsigned char)s[i + 1];
         if ((c == 0xe0 && second < 0xa0) || (c == 0xed && second > 0x9f))
            return 0;
      }
      if (need == 4)
      {
         unsigned char second = (unsigned char)s[i + 1];
         if ((c == 0xf0 && second < 0x90) || (c == 0xf4 && second > 0x8f))
            return 0;
      }

      i += need;
   }
   return 1;
}

size_t text_utf8_chunk_len(const char *s, size_t remaining, size_t max_bytes)
{
   if (!s || remaining == 0 || max_bytes == 0)
      return 0;

   size_t take = remaining < max_bytes ? remaining : max_bytes;
   if (take == remaining)
      return take;

   /* `take` is the first byte excluded from the chunk. If it is a continuation
    * byte, walk back to the lead byte and leave the whole code point for the
    * next chunk. If it is ASCII or a lead byte, the prefix already ends at a
    * character boundary. */
   while (take > 0 && ((unsigned char)s[take] & 0xc0) == 0x80)
      take--;
   return take;
}

/* --- stem_word --- */

/* --- canonical_fingerprint --- */

/* --- word_similarity --- */

/* --- is_contradiction --- */

/* --- split_camel_case --- */

int split_camel_case(const char *s, char **out, int max_parts)
{
   if (!s || !out || max_parts <= 0)
      return 0;

   int count = 0;
   const char *start = s;
   size_t len = strlen(s);

   for (size_t i = 1; i <= len && count < max_parts; i++)
   {
      int split = 0;
      if (i == len)
      {
         split = 1;
      }
      else if (isupper((unsigned char)s[i]) && !isupper((unsigned char)s[i - 1]))
      {
         split = 1;
      }
      else if (isupper((unsigned char)s[i]) && isupper((unsigned char)s[i - 1]) && i + 1 < len &&
               islower((unsigned char)s[i + 1]))
      {
         split = 1;
      }
      else if (s[i] == '_')
      {
         /* Split on underscore */
         size_t plen = (size_t)(&s[i] - start);
         if (plen > 0)
         {
            char *part = malloc(plen + 1);
            if (!part)
               return count;
            memcpy(part, start, plen);
            part[plen] = '\0';
            out[count++] = part;
         }
         start = &s[i + 1];
         continue;
      }

      if (split)
      {
         size_t plen = (size_t)(&s[i] - start);
         if (plen > 0)
         {
            char *part = malloc(plen + 1);
            if (!part)
               return count;
            memcpy(part, start, plen);
            part[plen] = '\0';
            out[count++] = part;
         }
         start = &s[i];
      }
   }

   return count;
}

/* --- tokenize_for_search --- */

int tokenize_for_search(const char *text, char **out, int max_tokens)
{
   if (!text || !out || max_tokens <= 0)
      return 0;

   char buf[4096];
   size_t bi = 0;

   /* Lowercase and replace non-alnum with space */
   for (size_t i = 0; text[i] && bi < sizeof(buf) - 1; i++)
   {
      if (isalnum((unsigned char)text[i]))
         buf[bi++] = (char)tolower((unsigned char)text[i]);
      else
         buf[bi++] = ' ';
   }
   buf[bi] = '\0';

   /* Split on spaces, filter stopwords and short tokens */
   int count = 0;
   char *sp = buf;
   while (*sp && count < max_tokens)
   {
      while (*sp == ' ')
         sp++;
      if (!*sp)
         break;

      char *start = sp;
      while (*sp && *sp != ' ')
         sp++;
      size_t tlen = (size_t)(sp - start);

      char tmp[256];
      if (tlen >= sizeof(tmp))
         tlen = sizeof(tmp) - 1;
      memcpy(tmp, start, tlen);
      tmp[tlen] = '\0';

      if (tlen >= 3 && !is_stopword(tmp))
      {
         /* Also split camelCase */
         char *parts[16];
         char joined[256];
         snprintf(joined, sizeof(joined), "%s", tmp);

         /* Check for camelCase in original */
         int has_upper = 0;
         for (size_t ci = 0; ci < tlen; ci++)
         {
            if (isupper((unsigned char)start[ci]))
            {
               has_upper = 1;
               break;
            }
         }

         if (!has_upper)
         {
            char *dup = strdup(tmp);
            if (!dup)
               break;
            out[count++] = dup;
         }
         else
         {
            /* Lowercase version already in tmp */
            char *dup = strdup(tmp);
            if (!dup)
               break;
            out[count++] = dup;

            /* Split camelCase parts from original case */
            char orig[256];
            memcpy(orig, start, tlen);
            orig[tlen] = '\0';
            int nparts = split_camel_case(orig, parts, 16);
            for (int pi = 0; pi < nparts && count < max_tokens; pi++)
            {
               /* Lowercase each part */
               for (char *cp = parts[pi]; *cp; cp++)
                  *cp = (char)tolower((unsigned char)*cp);
               if (strlen(parts[pi]) >= 3 && !is_stopword(parts[pi]))
                  out[count++] = strdup(parts[pi]);
               free(parts[pi]);
            }
         }
      }
   }

   return count;
}

/* --- expand_terms_for_search --- */
