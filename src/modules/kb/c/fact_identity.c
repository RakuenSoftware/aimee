/* db2/fact_identity.c: normalized identity for typed-fact assertions.
 * See fact_identity.h for what this guarantees and what it deliberately does
 * not. */

#include "fact_identity.h"
#include "../headers/rel_types.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fact_identity_unicode.h"

static int fi_u32_search(const uint32_t *values, size_t n, uint32_t cp)
{
   size_t lo = 0, hi = n;
   while (lo < hi)
   {
      size_t mid = lo + (hi - lo) / 2;
      if (values[mid] < cp)
         lo = mid + 1;
      else
         hi = mid;
   }
   return lo < n && values[lo] == cp;
}

static const fi_unicode_map_t *fi_mapping(const fi_unicode_map_t *map, size_t n, uint32_t cp)
{
   size_t lo = 0, hi = n;
   while (lo < hi)
   {
      size_t mid = lo + (hi - lo) / 2;
      if (map[mid].cp < cp)
         lo = mid + 1;
      else
         hi = mid;
   }
   return lo < n && map[lo].cp == cp ? &map[lo] : NULL;
}

static uint8_t fi_combining_class(uint32_t cp)
{
   size_t lo = 0, hi = FI_UNICODE_CCC_COUNT;
   while (lo < hi)
   {
      size_t mid = lo + (hi - lo) / 2;
      if (FI_UNICODE_CCC[mid].cp < cp)
         lo = mid + 1;
      else
         hi = mid;
   }
   return lo < FI_UNICODE_CCC_COUNT && FI_UNICODE_CCC[lo].cp == cp ? FI_UNICODE_CCC[lo].ccc : 0;
}

static uint32_t fi_compose_pair(uint32_t first, uint32_t second)
{
   /* Unicode's algorithmic Hangul composition. */
   enum
   {
      SBASE = 0xAC00,
      LBASE = 0x1100,
      VBASE = 0x1161,
      TBASE = 0x11A7,
      LCOUNT = 19,
      VCOUNT = 21,
      TCOUNT = 28,
      NCOUNT = VCOUNT * TCOUNT,
      SCOUNT = LCOUNT * NCOUNT
   };
   if (first >= LBASE && first < LBASE + LCOUNT && second >= VBASE && second < VBASE + VCOUNT)
      return SBASE + ((first - LBASE) * VCOUNT + (second - VBASE)) * TCOUNT;
   if (first >= SBASE && first < SBASE + SCOUNT && (first - SBASE) % TCOUNT == 0 &&
       second > TBASE && second < TBASE + TCOUNT)
      return first + second - TBASE;

   size_t lo = 0, hi = FI_UNICODE_COMPOSE_COUNT;
   while (lo < hi)
   {
      size_t mid = lo + (hi - lo) / 2;
      const fi_unicode_compose_t *row = &FI_UNICODE_COMPOSE[mid];
      if (row->first < first || (row->first == first && row->second < second))
         lo = mid + 1;
      else
         hi = mid;
   }
   if (lo < FI_UNICODE_COMPOSE_COUNT && FI_UNICODE_COMPOSE[lo].first == first &&
       FI_UNICODE_COMPOSE[lo].second == second)
      return FI_UNICODE_COMPOSE[lo].composed;
   return 0;
}

static int fi_decode(const unsigned char *s, uint32_t *cp, size_t *used)
{
   if (s[0] < 0x80)
   {
      *cp = s[0];
      *used = 1;
      return 1;
   }
   int n = (s[0] & 0xE0) == 0xC0 ? 2 : (s[0] & 0xF0) == 0xE0 ? 3 : (s[0] & 0xF8) == 0xF0 ? 4 : 0;
   if (!n)
      return 0;
   uint32_t v = s[0] & (uint32_t)(0x7F >> n);
   for (int i = 1; i < n; i++)
   {
      if ((s[i] & 0xC0) != 0x80)
         return 0;
      v = (v << 6) | (s[i] & 0x3F);
   }
   if ((n == 2 && v < 0x80) || (n == 3 && v < 0x800) || (n == 4 && v < 0x10000) || v > 0x10FFFF ||
       (v >= 0xD800 && v <= 0xDFFF))
      return 0;
   *cp = v;
   *used = (size_t)n;
   return 1;
}

static size_t fi_encode(uint32_t cp, char out[4])
{
   if (cp <= 0x7F)
   {
      out[0] = (char)cp;
      return 1;
   }
   if (cp <= 0x7FF)
   {
      out[0] = (char)(0xC0 | (cp >> 6));
      out[1] = (char)(0x80 | (cp & 0x3F));
      return 2;
   }
   if (cp <= 0xFFFF)
   {
      out[0] = (char)(0xE0 | (cp >> 12));
      out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[2] = (char)(0x80 | (cp & 0x3F));
      return 3;
   }
   out[0] = (char)(0xF0 | (cp >> 18));
   out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
   out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
   out[3] = (char)(0x80 | (cp & 0x3F));
   return 4;
}

size_t fact_identity_normalize_component(const char *in, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return 0;
   out[0] = '\0';
   if (!in)
      return 0;

   size_t in_bytes = strlen(in);
   if (in_bytes > (SIZE_MAX - 1) / 20)
      return 0;
   size_t cap = in_bytes * 20 + 1; /* Unicode 15.1 maximum NFKD expansion is 18. */
   uint32_t *normalized = malloc(cap * sizeof(*normalized));
   if (!normalized)
      return 0;
   size_t normalized_n = 0;
   const unsigned char *p = (const unsigned char *)in;
   while (*p)
   {
      uint32_t cp = 0;
      size_t used = 0;
      if (!fi_decode(p, &cp, &used))
         goto fail;
      p += used;
      if (cp >= 0xAC00 && cp <= 0xD7A3)
      {
         uint32_t index = cp - 0xAC00;
         uint32_t parts[3] = {0x1100 + index / 588, 0x1161 + (index % 588) / 28,
                              0x11A7 + index % 28};
         size_t part_n = parts[2] == 0x11A7 ? 2 : 3;
         for (size_t i = 0; i < part_n; i++)
            normalized[normalized_n++] = parts[i];
         continue;
      }
      const fi_unicode_map_t *decomp = fi_mapping(FI_NFKD_MAP, FI_NFKD_MAP_COUNT, cp);
      size_t count = decomp ? decomp->len : 1;
      for (size_t i = 0; i < count; i++)
      {
         if (normalized_n >= cap)
            goto fail;
         uint32_t part = decomp ? FI_NFKD_POOL[decomp->offset + i] : cp;
         normalized[normalized_n++] = part;
         uint8_t ccc = fi_combining_class(part);
         size_t pos = normalized_n - 1;
         while (ccc && pos > 0)
         {
            uint8_t prev = fi_combining_class(normalized[pos - 1]);
            if (prev == 0 || prev <= ccc)
               break;
            normalized[pos] = normalized[pos - 1];
            normalized[pos - 1] = part;
            pos--;
         }
      }
   }

   /* Canonical composition completes NFKC after compatibility decomposition. */
   if (normalized_n > 1)
   {
      size_t write = 1, starter = 0;
      uint8_t last_cc = 0;
      for (size_t read = 1; read < normalized_n; read++)
      {
         uint32_t cp = normalized[read];
         uint8_t cc = fi_combining_class(cp);
         uint32_t composite =
             (last_cc < cc || last_cc == 0) ? fi_compose_pair(normalized[starter], cp) : 0;
         if (composite)
            normalized[starter] = composite;
         else
         {
            if (cc == 0)
               starter = write;
            normalized[write++] = cp;
            last_cc = cc;
         }
      }
      normalized_n = write;
   }

   size_t o = 0;
   int pending_space = 0;
   int seen = 0;
   for (size_t ni = 0; ni < normalized_n; ni++)
   {
      uint32_t cp = normalized[ni];
      const fi_unicode_map_t *mapping = fi_mapping(FI_CASEFOLD_MAP, FI_CASEFOLD_MAP_COUNT, cp);
      size_t mapped_n = mapping ? mapping->len : 1;
      for (size_t mi = 0; mi < mapped_n; mi++)
      {
         uint32_t mapped = mapping ? FI_CASEFOLD_POOL[mapping->offset + mi] : cp;
         if (fi_u32_search(FI_UNICODE_SPACES, FI_UNICODE_SPACES_COUNT, mapped))
         {
            if (seen)
               pending_space = 1;
            continue;
         }
         char encoded[4];
         size_t encoded_n = fi_encode(mapped, encoded);
         size_t needed = encoded_n + (pending_space ? 1u : 0u);
         if (o + needed >= out_len)
         {
            goto fail; /* a truncated identity would alias unrelated facts */
         }
         if (pending_space)
            out[o++] = ' ';
         memcpy(out + o, encoded, encoded_n);
         o += encoded_n;
         pending_space = 0;
         seen = 1;
      }
   }

   out[o] = '\0';
   free(normalized);
   return o;

fail:
   free(normalized);
   out[0] = '\0';
   return 0;
}

size_t fact_identity_subject_key(const char *source, const char *relation, char *out,
                                 size_t out_len)
{
   if (!out || out_len == 0)
      return 0;
   out[0] = '\0';
   if (!source || !relation)
      return 0;

   char ns[FACT_IDENTITY_KEY_MAX];
   char nr[REL_TYPE_NAME_MAX];
   if (fact_identity_normalize_component(source, ns, sizeof(ns)) == 0)
      return 0;
   rel_type_normalize(relation, nr, sizeof(nr));
   if (!nr[0])
      return 0;

   int wrote = snprintf(out, out_len, "%s\x1f%s", ns, nr);
   if (wrote <= 0 || (size_t)wrote >= out_len)
   {
      out[0] = '\0';
      return 0;
   }
   return (size_t)wrote;
}

size_t fact_identity_key(const char *source, const char *relation, const char *target, char *out,
                         size_t out_len)
{
   if (!out || out_len == 0)
      return 0;
   out[0] = '\0';
   if (!source || !relation || !target)
      return 0;

   char ns[FACT_IDENTITY_KEY_MAX];
   char nt[FACT_IDENTITY_KEY_MAX];
   char nr[REL_TYPE_NAME_MAX];

   if (fact_identity_normalize_component(source, ns, sizeof(ns)) == 0)
      return 0;
   if (fact_identity_normalize_component(target, nt, sizeof(nt)) == 0)
      return 0;

   /* The predicate goes through the existing relation normalizer so one
    * relation is spelled one way everywhere, rather than this unit inventing a
    * second spelling of the same thing. */
   rel_type_normalize(relation, nr, sizeof(nr));
   if (!nr[0])
      return 0;

   /* U+001F (unit separator) joins the parts. Whitespace has already been
    * collapsed to plain spaces and control bytes are not whitespace, so this
    * byte cannot occur inside a normalized component -- which is what stops
    * ("a b", "c") and ("a", "b c") from colliding. */
   int wrote = snprintf(out, out_len, "%s\x1f%s\x1f%s", ns, nr, nt);
   if (wrote <= 0 || (size_t)wrote >= out_len)
   {
      out[0] = '\0';
      return 0;
   }
   return (size_t)wrote;
}
