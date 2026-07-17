/* fold_recall.c: page folded content back in on re-touch (fold §4, P4).
 * See fold_recall.h. Pure: dstr + libc only. */
#include "fold_recall.h"

#include <stdlib.h>
#include <string.h>

void fold_recall_index_init(fold_recall_index_t *ix)
{
   if (!ix)
      return;
   ix->keys = NULL;
   ix->last_turn = NULL;
   ix->count = 0;
   ix->cap = 0;
}

void fold_recall_index_free(fold_recall_index_t *ix)
{
   if (!ix)
      return;
   for (size_t i = 0; i < ix->count; i++)
      free(ix->keys[i]);
   free(ix->keys);
   free(ix->last_turn);
   fold_recall_index_init(ix);
}

/* A character that can be part of a coordinate token (path / handle / id). */
static int is_coord_char(int c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' ||
          c == '.' || c == '_' || c == '-' || c == ':';
}

/* Whole-token containment: `key` must appear in `hay` not extended by a coordinate
 * char on either side, so "/x" does not match "/xyz" and "memory:42" does not match
 * "memory:420" (avoids spurious recall hints from substring collisions). */
static int contains_token(const char *hay, const char *key)
{
   size_t kl = strlen(key);
   if (kl == 0)
      return 0;
   const char *p = hay;
   while ((p = strstr(p, key)) != NULL)
   {
      int before_ok = (p == hay) || !is_coord_char((unsigned char)p[-1]);
      int after_ok = !is_coord_char((unsigned char)p[kl]);
      if (before_ok && after_ok)
         return 1;
      p += 1;
   }
   return 0;
}

static int index_has(const fold_recall_index_t *ix, const char *key)
{
   for (size_t i = 0; i < ix->count; i++)
      if (strcmp(ix->keys[i], key) == 0)
         return 1;
   return 0;
}

void fold_recall_index_add(fold_recall_index_t *ix, const char *key)
{
   if (!ix || !key || !key[0])
      return;
   if (index_has(ix, key))
      return;
   if (ix->count == ix->cap)
   {
      size_t ncap = ix->cap ? ix->cap * 2 : 8;
      /* Commit each realloc to ix as it succeeds (realloc frees the old block, so
       * the old pointer must not be kept). If the second realloc fails, ix->keys
       * is grown but ix->cap stays at the old (smaller) size — consistent and
       * OOB-safe, since all writes are bounded by ix->cap. */
      char **nk = realloc(ix->keys, ncap * sizeof(*nk));
      if (!nk)
         return;
      ix->keys = nk;
      int *nt = realloc(ix->last_turn, ncap * sizeof(*nt));
      if (!nt)
         return;
      ix->last_turn = nt;
      ix->cap = ncap;
   }
   size_t n = strlen(key);
   char *copy = malloc(n + 1);
   if (!copy)
      return;
   memcpy(copy, key, n + 1);
   ix->keys[ix->count] = copy;
   ix->last_turn[ix->count] = -1;
   ix->count++;
}

size_t fold_recall_detect(fold_recall_index_t *ix, const char *turn_text, int turn, int ttl_turns,
                          dstr_t *out)
{
   if (!ix || !turn_text || !turn_text[0])
      return 0;
   if (ttl_turns <= 0)
      ttl_turns = FOLD_RECALL_DEFAULT_TTL_TURNS;

   size_t surfaced = 0;
   for (size_t i = 0; i < ix->count; i++)
   {
      const char *key = ix->keys[i];
      if (!contains_token(turn_text, key))
         continue; /* not re-touched this turn (whole-token match) */
      int last = ix->last_turn[i];
      if (last >= 0 && (turn - last) < ttl_turns)
         continue; /* surfaced too recently — anti-thrash */
      if (out)
         dstr_appendf(
             out, "  recall: %s was folded — page it back in via code_span_get/memory_get\n", key);
      ix->last_turn[i] = turn;
      surfaced++;
   }
   return surfaced;
}
