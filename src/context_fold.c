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

int context_fold_view(const cJSON *messages, const fold_config_t *cfg, fold_result_t *out)
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

   int desired = count - retained;
   int split = -1;
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

   /* size of the folded region (bounds the closet ratio cap) */
   size_t folded_bytes = 0;
   for (int i = 0; i < split; i++)
   {
      cJSON *it = cJSON_GetArrayItem((cJSON *)messages, i);
      if (!it)
         continue;
      char *s = cJSON_PrintUnformatted(it);
      if (s)
      {
         folded_bytes += strlen(s);
         free(s);
      }
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

   out->messages = arr;
   out->folded = 1;
   out->folded_msgs = split;
   out->retained_msgs = count - split;
   return 0;
}
