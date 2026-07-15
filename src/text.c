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

static int extract_trigrams(const char *s, trigram_t *out, int max)
{
   int count = 0;
   size_t len = strlen(s);
   if (len < 3)
   {
      if (len > 0 && count < max)
      {
         memset(out[count].tri, 0, 4);
         strncpy(out[count].tri, s, 3);
         count++;
      }
      return count;
   }
   for (size_t i = 0; i <= len - 3 && count < max; i++)
   {
      out[count].tri[0] = (char)tolower((unsigned char)s[i]);
      out[count].tri[1] = (char)tolower((unsigned char)s[i + 1]);
      out[count].tri[2] = (char)tolower((unsigned char)s[i + 2]);
      out[count].tri[3] = '\0';
      count++;
   }
   return count;
}

static int trigram_in_set(const char *tri, trigram_t *set, int count)
{
   for (int i = 0; i < count; i++)
   {
      if (memcmp(tri, set[i].tri, 3) == 0)
         return 1;
   }
   return 0;
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

double trigram_similarity(const char *a, const char *b)
{
   if (!a || !b || !a[0] || !b[0])
      return 0.0;

   trigram_t ta[512], tb[512];
   int na = extract_trigrams(a, ta, 512);
   int nb = extract_trigrams(b, tb, 512);

   if (na == 0 && nb == 0)
      return 1.0;
   if (na == 0 || nb == 0)
      return 0.0;

   int intersection = 0;
   for (int i = 0; i < na; i++)
   {
      if (trigram_in_set(ta[i].tri, tb, nb))
         intersection++;
   }

   /* Jaccard = intersection / union, union = na + nb - intersection */
   int uni = na + nb - intersection;
   if (uni == 0)
      return 0.0;
   return (double)intersection / (double)uni;
}

/* --- stem_word --- */

static int ends_with(const char *word, size_t wlen, const char *suffix, size_t slen)
{
   if (wlen < slen)
      return 0;
   return memcmp(word + wlen - slen, suffix, slen) == 0;
}

char *stem_word(const char *word, char *buf, size_t buf_len)
{
   if (!word || !buf || buf_len == 0)
   {
      if (buf && buf_len > 0)
         buf[0] = '\0';
      return buf;
   }

   size_t wlen = strlen(word);
   if (wlen == 0 || wlen >= buf_len)
   {
      snprintf(buf, buf_len, "%s", word);
      return buf;
   }

   /* Copy to buf for in-place modification */
   memcpy(buf, word, wlen + 1);

   /* Minimum stem length: keep at least 3 chars */
#define MIN_STEM 3

   /* Try suffixes longest-first */
   struct
   {
      const char *suffix;
      size_t len;
      const char *replace;
      size_t rlen;
   } rules[] = {
       {"tion", 4, "t", 1},  {"sion", 4, "s", 1}, {"ment", 4, "", 0},  {"ness", 4, "", 0},
       {"ying", 4, "y", 1},  {"ling", 4, "l", 1}, {"ting", 4, "t", 1}, {"sing", 4, "s", 1},
       {"ring", 4, "r", 1},  {"ning", 4, "n", 1}, {"ding", 4, "d", 1}, {"ping", 4, "p", 1},
       {"ving", 4, "ve", 2}, {"ful", 3, "", 0},   {"ous", 3, "", 0},   {"ive", 3, "", 0},
       {"ing", 3, "", 0},    {"ied", 3, "y", 1},  {"ies", 3, "y", 1},  {"ed", 2, "", 0},
       {"es", 2, "", 0},     {"ly", 2, "", 0},    {"al", 2, "", 0},    {"er", 2, "", 0},
       {"s", 1, "", 0},
   };

   int nrules = (int)(sizeof(rules) / sizeof(rules[0]));
   for (int i = 0; i < nrules; i++)
   {
      if (wlen > rules[i].len + MIN_STEM - rules[i].rlen &&
          ends_with(buf, wlen, rules[i].suffix, rules[i].len))
      {
         size_t stem_len = wlen - rules[i].len;
         if (stem_len + rules[i].rlen >= MIN_STEM)
         {
            memcpy(buf + stem_len, rules[i].replace, rules[i].rlen);
            buf[stem_len + rules[i].rlen] = '\0';
            return buf;
         }
      }
   }

   return buf;
}

/* --- canonical_fingerprint --- */

static int cmp_str(const void *a, const void *b)
{
   return strcmp(*(const char **)a, *(const char **)b);
}

char *canonical_fingerprint(const char *text, char *buf, size_t buf_len)
{
   if (!text || !buf || buf_len == 0)
   {
      if (buf && buf_len > 0)
         buf[0] = '\0';
      return buf;
   }

   /* Normalize first */
   char normed[4096];
   normalize_key(text, normed, sizeof(normed));

   /* Split into words, stem each */
   char *words[256] = {0};
   char stems[256][64];
   int wcount = 0;

   char work[4096];
   snprintf(work, sizeof(work), "%s", normed);
   char *save1;
   char *tok = strtok_r(work, " ", &save1);
   while (tok && wcount < 256)
   {
      stem_word(tok, stems[wcount], 64);
      words[wcount] = stems[wcount];
      wcount++;
      tok = strtok_r(NULL, " ", &save1);
   }

   /* Sort */
   qsort(words, (size_t)wcount, sizeof(char *), cmp_str);

   /* Join */
   size_t out = 0;
   for (int i = 0; i < wcount; i++)
   {
      if (i > 0 && out < buf_len - 1)
         buf[out++] = ' ';
      size_t wl = strlen(words[i]);
      if (out + wl < buf_len)
      {
         memcpy(buf + out, words[i], wl);
         out += wl;
      }
   }
   buf[out] = '\0';
   return buf;
}

/* --- word_similarity --- */

double word_similarity(const char *a, const char *b)
{
   if (!a || !b || !a[0] || !b[0])
      return 0.0;

   /* Tokenize both */
   char wa[4096], wb[4096];
   snprintf(wa, sizeof(wa), "%s", a);
   snprintf(wb, sizeof(wb), "%s", b);

   char *words_a[256], *words_b[256];
   int na = 0, nb = 0;

   for (char *p = wa; *p; p++)
      *p = (char)tolower((unsigned char)*p);
   for (char *p = wb; *p; p++)
      *p = (char)tolower((unsigned char)*p);

   char *save2;
   char *tok = strtok_r(wa, " \t\n\r", &save2);
   while (tok && na < 256)
   {
      words_a[na++] = tok;
      tok = strtok_r(NULL, " \t\n\r", &save2);
   }

   /* Manual splitting for b */
   char *p = wb;
   while (*p && nb < 256)
   {
      while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
         p++;
      if (!*p)
         break;
      words_b[nb++] = p;
      while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
         p++;
      if (*p)
         *p++ = '\0';
   }

   if (na == 0 && nb == 0)
      return 1.0;
   if (na == 0 || nb == 0)
      return 0.0;

   /* Count intersection */
   int intersection = 0;
   for (int i = 0; i < na; i++)
   {
      for (int j = 0; j < nb; j++)
      {
         if (strcmp(words_a[i], words_b[j]) == 0)
         {
            intersection++;
            break;
         }
      }
   }

   /* Union: count unique across both sets */
   int uni = nb;
   for (int i = 0; i < na; i++)
   {
      int found = 0;
      for (int j = 0; j < nb; j++)
      {
         if (strcmp(words_a[i], words_b[j]) == 0)
         {
            found = 1;
            break;
         }
      }
      if (!found)
         uni++;
   }

   if (uni == 0)
      return 0.0;
   return (double)intersection / (double)uni;
}

/* --- is_contradiction --- */

static const char *negators[] = {
    "never",  "don't",   "dont",   "not",    "no",     "shouldn't", "shouldnt", "can't", "cant",
    "won't",  "wont",    "isn't",  "isnt",   "aren't", "arent",     "wasn't",   "wasnt", "weren't",
    "werent", "doesn't", "doesnt", "didn't", "didnt",  "avoid",     "disable",  NULL};

static int has_negator(const char *text)
{
   char buf[4096];
   snprintf(buf, sizeof(buf), "%s", text);
   for (char *p = buf; *p; p++)
      *p = (char)tolower((unsigned char)*p);

   for (int i = 0; negators[i]; i++)
   {
      if (strstr(buf, negators[i]))
         return 1;
   }
   return 0;
}

static void strip_negators(const char *text, char *buf, size_t buf_len)
{
   char tmp[4096];
   snprintf(tmp, sizeof(tmp), "%s", text);
   for (char *p = tmp; *p; p++)
      *p = (char)tolower((unsigned char)*p);

   size_t out = 0;
   char *save3;
   char *tok = strtok_r(tmp, " \t\n\r", &save3);
   while (tok)
   {
      int is_neg = 0;
      for (int i = 0; negators[i]; i++)
      {
         if (strcmp(tok, negators[i]) == 0)
         {
            is_neg = 1;
            break;
         }
      }
      if (!is_neg)
      {
         if (out > 0 && out < buf_len - 1)
            buf[out++] = ' ';
         size_t tl = strlen(tok);
         if (out + tl < buf_len)
         {
            memcpy(buf + out, tok, tl);
            out += tl;
         }
      }
      tok = strtok_r(NULL, " \t\n\r", &save3);
   }
   buf[out] = '\0';
}

int is_contradiction(const char *a, const char *b)
{
   if (!a || !b)
      return 0;

   int neg_a = has_negator(a);
   int neg_b = has_negator(b);

   /* One must negate, the other must not */
   if (neg_a == neg_b)
      return 0;

   /* Check word similarity on cleaned text (negators removed) */
   char clean_a[4096], clean_b[4096];
   strip_negators(a, clean_a, sizeof(clean_a));
   strip_negators(b, clean_b, sizeof(clean_b));

   return word_similarity(clean_a, clean_b) > 0.5;
}

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

char *expand_terms_for_search(char **terms, int count, char *buf, size_t buf_len)
{
   if (!terms || !buf || buf_len == 0)
   {
      if (buf && buf_len > 0)
         buf[0] = '\0';
      return buf;
   }

   size_t out = 0;
   for (int i = 0; i < count; i++)
   {
      if (!terms[i])
         continue;

      /* Add original term */
      size_t tlen = strlen(terms[i]);
      if (out + tlen + 1 < buf_len)
      {
         if (out > 0)
            buf[out++] = ' ';
         memcpy(buf + out, terms[i], tlen);
         out += tlen;
      }

      /* Split camelCase/snake_case and add parts */
      char *parts[16];
      int nparts = split_camel_case(terms[i], parts, 16);
      for (int pi = 0; pi < nparts; pi++)
      {
         /* Lowercase */
         for (char *cp = parts[pi]; *cp; cp++)
            *cp = (char)tolower((unsigned char)*cp);

         size_t plen = strlen(parts[pi]);
         if (plen >= 2 && out + plen + 1 < buf_len)
         {
            /* Check not duplicate of the original */
            char lower_orig[256];
            snprintf(lower_orig, sizeof(lower_orig), "%s", terms[i]);
            for (char *cp = lower_orig; *cp; cp++)
               *cp = (char)tolower((unsigned char)*cp);
            if (strcmp(parts[pi], lower_orig) != 0)
            {
               buf[out++] = ' ';
               memcpy(buf + out, parts[pi], plen);
               out += plen;
            }
         }
         free(parts[pi]);
      }
   }
   buf[out] = '\0';
   return buf;
}

/* --- Negation-scope tokeniser --- */

int is_negation_marker(const char *word)
{
   if (!word)
      return 0;
   static const char *markers[] = {"not",    "never",   "no",      "without", "haven't", "hasn't",
                                   "didn't", "doesn't", "can't",   "won't",   "neither", "nor",
                                   "nobody", "nothing", "nowhere", "none",    NULL};
   for (int i = 0; markers[i]; i++)
   {
      if (strcmp(word, markers[i]) == 0)
         return 1;
   }
   return 0;
}

/* Tokenise text into words (ASCII alphanum + apostrophe), lowercased.
 * Fills toks[0..max-1]; returns count.  Caller provides storage. */
static int neg_tokenise(const char *text, char toks[][64], int max)
{
   if (!text)
      return 0;
   int count = 0;
   const char *p = text;
   while (*p && count < max)
   {
      /* skip non-word */
      while (*p && !isalnum((unsigned char)*p) && *p != '\'')
         p++;
      if (!*p)
         break;
      /* collect word */
      int j = 0;
      while (*p && (isalnum((unsigned char)*p) || *p == '\'') && j < 63)
      {
         toks[count][j++] = (char)tolower((unsigned char)*p);
         p++;
      }
      toks[count][j] = '\0';
      if (j >= 2)
         count++;
   }
   return count;
}

int extract_negation_tokens(const char *text, char *buf, size_t buf_len)
{
   if (!text || !buf || buf_len == 0)
      return 0;
   buf[0] = '\0';

   enum
   {
      NEG_MAX_TOKS = 256
   };
   char toks[NEG_MAX_TOKS][64];
   int n = neg_tokenise(text, toks, NEG_MAX_TOKS);
   if (n == 0)
      return 0;

   /* Mark tokens that are negation markers */
   int is_neg[NEG_MAX_TOKS];
   memset(is_neg, 0, sizeof(int) * (size_t)n);
   for (int i = 0; i < n; i++)
      is_neg[i] = is_negation_marker(toks[i]);

   /* Compute token offsets in text for clause-boundary detection */
   int tok_start[NEG_MAX_TOKS];
   memset(tok_start, 0, sizeof(int) * (size_t)n);
   {
      const char *p = text;
      int ti = 0;
      while (*p && ti < n)
      {
         while (*p && !isalnum((unsigned char)*p) && *p != '\'')
            p++;
         if (!*p)
            break;
         tok_start[ti] = (int)(p - text);
         while (*p && (isalnum((unsigned char)*p) || *p == '\''))
            p++;
         ti++;
      }
   }

   size_t out_pos = 0;
   int written = 0;

   for (int i = 0; i < n; i++)
   {
      /* Skip stopwords, short tokens, and negation markers themselves */
      if (is_neg[i] || is_stopword(toks[i]) || strlen(toks[i]) < 3)
         continue;

      /* Check if any negation marker is within ±3 positions, same clause */
      int in_scope = 0;
      for (int d = -3; d <= 3 && !in_scope; d++)
      {
         if (d == 0)
            continue;
         int j = i + d;
         if (j < 0 || j >= n)
            continue;
         if (!is_neg[j])
            continue;
         /* Check no clause-boundary punctuation between j and i */
         int lo_pos = j < i ? tok_start[j] : tok_start[i];
         int hi_pos = j < i ? tok_start[i] : tok_start[j];
         int boundary = 0;
         for (int k = lo_pos; k < hi_pos && !boundary; k++)
         {
            char c = text[k];
            if (c == '.' || c == '!' || c == '?' || c == ';' || c == ':')
               boundary = 1;
         }
         if (!boundary)
            in_scope = 1;
      }

      if (!in_scope)
         continue;

      /* Emit "not_<token>" */
      char synthetic[72];
      snprintf(synthetic, sizeof(synthetic), "not_%s", toks[i]);
      size_t slen = strlen(synthetic);
      if (out_pos + slen + 2 <= buf_len)
      {
         if (out_pos > 0)
            buf[out_pos++] = ' ';
         memcpy(buf + out_pos, synthetic, slen);
         out_pos += slen;
         written++;
      }
   }
   buf[out_pos] = '\0';
   return written;
}

polarity_t memory_query_polarity(const char *query)
{
   if (!query)
      return POLARITY_POSITIVE;
   enum
   {
      NEG_QUERY_MAX_TOKS = 64
   };
   char toks[NEG_QUERY_MAX_TOKS][64];
   int n = neg_tokenise(query, toks, NEG_QUERY_MAX_TOKS);
   for (int i = 0; i < n; i++)
   {
      if (is_negation_marker(toks[i]))
         return POLARITY_NEGATIVE;
   }
   return POLARITY_POSITIVE;
}
