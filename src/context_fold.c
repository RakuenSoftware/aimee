/* context_fold.c: deterministic rolling fold of old conversation turns (§1, P2a).
 * See context_fold.h for the contract. Pure: cJSON + dstr + coord_closet only. */
#include "context_fold.h"

#include "dstr.h"
#include <stdlib.h>
#include <string.h>

/* A "clean user turn": role=user and NOT a tool_result message. Folding only at
 * such a boundary guarantees a tool_use and its tool_result never split, and the
 * retained tail begins with a user message (valid alternation). */
static int is_clean_user_turn(const cJSON *m)
{
   const char *role = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)m, "role"));
   if (!role || strcmp(role, "user") != 0)
      return 0;
   cJSON *content = cJSON_GetObjectItem((cJSON *)m, "content");
   if (cJSON_IsString(content))
      return 1;
   if (cJSON_IsArray(content))
   {
      cJSON *b;
      cJSON_ArrayForEach(b, content)
      {
         const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(b, "type"));
         if (t && strcmp(t, "tool_result") == 0)
            return 0;
      }
      return 1;
   }
   return 0;
}

/* Append up to `max` bytes of `s` to `d`, single-lined; mark truncation with "…".
 * If the byte cap would land mid-UTF-8-sequence, back up to a character boundary
 * so the emitted JSON string value never contains a split multibyte char. */
static void append_excerpt(dstr_t *d, const char *s, int max)
{
   if (!s)
      return;
   int len = 0;
   while (s[len])
      len++;
   int n = len < max ? len : max;
   if (n < len)
   {
      /* truncating: back up off any continuation byte (0x80-0xBF) so [0,n) ends
       * on a complete character */
      while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
         n--;
   }
   for (int i = 0; i < n; i++)
   {
      char c = s[i];
      if (c == '\n' || c == '\r' || c == '\t')
         c = ' ';
      dstr_append_char(d, c);
   }
   if (n < len)
      dstr_append_str(d, "\xE2\x80\xA6"); /* ellipsis */
}

/* Emit one skeleton line (or lines) for a folded message; nominate its
 * identifiers into `set` with turn-indexed provenance. */
static void skeleton_message(dstr_t *d, const cJSON *m, int turn, int excerpt, coord_set_t *set)
{
   const char *role = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)m, "role"));
   cJSON *content = cJSON_GetObjectItem((cJSON *)m, "content");
   coord_provenance_t prov = {COORD_LANE_AGENT, turn, -1, -1};

   if (cJSON_IsString(content))
   {
      const char *txt = content->valuestring;
      if (txt)
      {
         coord_closet_nominate(txt, strlen(txt), &prov, set);
         dstr_appendf(d, "%s: ", role ? role : "?");
         append_excerpt(d, txt, excerpt);
         dstr_append_char(d, '\n');
      }
      return;
   }
   if (!cJSON_IsArray(content))
      return;

   cJSON *b;
   cJSON_ArrayForEach(b, content)
   {
      const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(b, "type"));
      if (!t)
         continue;
      if (strcmp(t, "text") == 0)
      {
         const char *txt = cJSON_GetStringValue(cJSON_GetObjectItem(b, "text"));
         if (txt)
         {
            coord_closet_nominate(txt, strlen(txt), &prov, set);
            dstr_appendf(d, "%s: ", role ? role : "?");
            append_excerpt(d, txt, excerpt);
            dstr_append_char(d, '\n');
         }
      }
      else if (strcmp(t, "tool_use") == 0)
      {
         const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(b, "name"));
         cJSON *input = cJSON_GetObjectItem(b, "input");
         char *inp = input ? cJSON_PrintUnformatted(input) : NULL;
         if (inp)
            coord_closet_nominate(inp, strlen(inp), &prov, set);
         dstr_appendf(d, "  $ %s ", name ? name : "tool");
         append_excerpt(d, inp ? inp : "", excerpt);
         dstr_append_char(d, '\n');
         free(inp);
      }
      else if (strcmp(t, "tool_result") == 0)
      {
         cJSON *c = cJSON_GetObjectItem(b, "content");
         char *owned = NULL;
         const char *cv = NULL;
         if (cJSON_IsString(c))
            cv = c->valuestring;
         else if (c)
         {
            owned = cJSON_PrintUnformatted(c);
            cv = owned;
         }
         if (cv)
         {
            coord_closet_nominate(cv, strlen(cv), &prov, set);
            dstr_append_str(d, "    \xE2\x86\x92 "); /* arrow */
            append_excerpt(d, cv, excerpt);
            dstr_appendf(d, " (%zu bytes)\n", strlen(cv));
         }
         free(owned);
      }
   }
}

void fold_result_free(fold_result_t *out)
{
   if (!out)
      return;
   if (out->messages)
      cJSON_Delete(out->messages);
   out->messages = NULL;
   out->folded = 0;
}

/* FNV-1a over the serialized bytes of messages[0..n); also returns the total
 * serialized size via *bytes. Detects prefix mutation for freeze reuse and bounds
 * the closet ratio cap in one pass. */
static unsigned long long prefix_digest(const cJSON *messages, int n, size_t *bytes)
{
   unsigned long long h = 14695981039346656037ULL;
   size_t total = 0;
   for (int i = 0; i < n; i++)
   {
      cJSON *it = cJSON_GetArrayItem((cJSON *)messages, i);
      if (!it)
         continue;
      char *s = cJSON_PrintUnformatted(it);
      if (!s)
         continue;
      for (const unsigned char *p = (const unsigned char *)s; *p; p++)
      {
         h ^= (unsigned long long)*p;
         h *= 1099511628211ULL;
      }
      total += strlen(s);
      free(s);
   }
   if (bytes)
      *bytes = total;
   return h;
}

int context_fold_view(const cJSON *messages, const fold_config_t *cfg, fold_freeze_t *freeze,
                      fold_result_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   out->closet_evict = COORD_EVICT_NONE;
   if (!messages || !cJSON_IsArray((cJSON *)messages) || !cfg || !cfg->enabled)
      return 0;

   int count = cJSON_GetArraySize((cJSON *)messages);
   int retained = cfg->retained_msgs > 0 ? cfg->retained_msgs : CONTEXT_FOLD_DEFAULT_RETAINED_MSGS;
   int min_fold = cfg->min_fold_msgs > 0 ? cfg->min_fold_msgs : CONTEXT_FOLD_DEFAULT_MIN_FOLD_MSGS;
   int excerpt = cfg->reasoning_excerpt_bytes > 0 ? cfg->reasoning_excerpt_bytes
                                                  : CONTEXT_FOLD_DEFAULT_EXCERPT_BYTES;

   /* Overflow-safe: never compute retained + min_fold (both could be near
    * INT_MAX from pathological cfg). */
   if (count <= 0 || retained >= count || count - retained < min_fold)
      return 0; /* too short to fold cleanly */

   int tail_cap = CONTEXT_FOLD_DEFAULT_TAIL_CAP_MSGS;
   if (freeze && freeze->tail_cap_msgs > 0)
      tail_cap = freeze->tail_cap_msgs;
   if (tail_cap < retained)
      tail_cap = retained; /* a cap below the retained band would re-epoch every turn */

   int split = -1;
   int reused = 0;
   size_t folded_bytes = 0;    /* serialized size of the folded region */
   unsigned long long dig = 0; /* digest of the folded region */

   /* §3 fold-freeze: reuse the pinned boundary only when it is still a clean
    * boundary, the tail is within cap, AND the folded prefix is byte-for-byte
    * unchanged. A mid-run compaction can mutate messages[0..frozen_split) while
    * preserving indices — the digest check turns that into an epoch (re-fold)
    * instead of a false "reuse" that would claim a warm cache it does not have. */
   if (freeze && freeze->active)
   {
      int fs = freeze->frozen_split;
      if (fs >= min_fold && fs < count && (count - fs) <= tail_cap &&
          is_clean_user_turn(cJSON_GetArrayItem((cJSON *)messages, fs)))
      {
         dig = prefix_digest(messages, fs, &folded_bytes);
         if (dig == freeze->prefix_digest)
         {
            split = fs;
            reused = 1;
         }
      }
   }

   if (!reused)
   {
      /* fresh boundary: first fold, freeze disabled, or an epoch advance */
      int desired = count - retained;
      for (int s = desired; s >= min_fold; s--)
      {
         if (is_clean_user_turn(cJSON_GetArrayItem((cJSON *)messages, s)))
         {
            split = s;
            break;
         }
      }
      if (split < min_fold)
         return 0; /* no clean boundary leaves enough folded */
      dig = prefix_digest(messages, split, &folded_bytes);
   }

   coord_set_t set;
   coord_set_init(&set);
   dstr_t body;
   dstr_init(&body);
   dstr_appendf(
       &body,
       "[folded %d earlier message(s); skeleton below — exact identifiers are conserved in "
       "the Coordinate Closet, full bodies remain in history]\n\n",
       split);
   for (int i = 0; i < split; i++)
      skeleton_message(&body, cJSON_GetArrayItem((cJSON *)messages, i), i, excerpt, &set);

   char *closet = coord_closet_render(&set, &cfg->closet, folded_bytes, &out->closet_evict);
   if (closet)
   {
      dstr_append_char(&body, '\n');
      dstr_append_str(&body, closet);
      free(closet);
   }
   coord_set_free(&set);

   /* Build the synthetic view. On any allocation failure, clean up and report
    * no-fold rather than emitting a partial/NULL-bearing array. */
   cJSON *arr = cJSON_CreateArray();
   cJSON *fm = cJSON_CreateObject();
   cJSON *ack = cJSON_CreateObject();
   if (!arr || !fm || !ack)
   {
      cJSON_Delete(arr);
      cJSON_Delete(fm);
      cJSON_Delete(ack);
      dstr_free(&body);
      return 0; /* OOM: caller uses the original transcript */
   }
   cJSON_AddStringToObject(fm, "role", "user");
   cJSON_AddStringToObject(fm, "content", dstr_cstr(&body));
   cJSON_AddItemToArray(arr, fm);
   cJSON_AddStringToObject(ack, "role", "assistant");
   cJSON_AddStringToObject(ack, "content",
                           "Understood — continuing from the folded summary above.");
   cJSON_AddItemToArray(arr, ack);
   dstr_free(&body);

   for (int i = split; i < count; i++)
   {
      cJSON *it = cJSON_GetArrayItem((cJSON *)messages, i);
      if (!it)
         continue;
      cJSON *dup = cJSON_Duplicate(it, 1);
      if (!dup) /* OOM mid-copy: abandon the fold cleanly */
      {
         cJSON_Delete(arr);
         return 0;
      }
      cJSON_AddItemToArray(arr, dup);
   }

   /* Commit the freeze state only after a successful build (an OOM return above
    * leaves the prior frozen boundary intact). epochs++ counts genuine boundary
    * advances, not no-op re-commits of the same boundary. */
   if (freeze)
   {
      int advanced = !reused && (!freeze->active || freeze->frozen_split != split ||
                                 freeze->prefix_digest != dig);
      freeze->active = 1;
      freeze->frozen_split = split;
      freeze->prefix_digest = dig;
      if (advanced)
         freeze->epochs++;
   }

   out->messages = arr;
   out->folded = 1;
   out->folded_msgs = split;
   out->retained_msgs = count - split;
   out->reused_boundary = reused;
   return 0;
}
