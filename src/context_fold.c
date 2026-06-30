/* context_fold.c: deterministic rolling fold of old conversation turns (§1, P2a).
 * See context_fold.h for the contract. Pure: cJSON + dstr + coord_closet only. */
#include "context_fold.h"

#include "compact.h" /* compact_body — the shared tool-result shrink core */
#include "dstr.h"
#include "fold_register.h"
#include <stdlib.h>
#include <string.h>

/* A "clean user turn": role=user and NOT a tool_result message. Folding only at
 * such a boundary guarantees a tool_use and its tool_result never split, and the
 * retained tail begins with a user message (valid alternation).
 *
 * Cross-provider safe: this only admits a boundary at role=="user" (and rejects an
 * Anthropic tool_result-carrying user msg). In OpenAI/Gemini a tool result is a
 * separate role=="tool" message and a role=="user" never interrupts an assistant
 * tool_calls -> tool result pair; in Responses the function_call/function_call_output
 * items are not role=="user" either. So folding at a user boundary never splits a
 * call/result pair in ANY provider shape — the boundary logic needs no per-format
 * branch. */
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
/* Emit the "role: " (or "role/register: ") prefix for a folded text line. */
static void skeleton_prefix(dstr_t *d, const char *role, const char *txt, int register_on)
{
   if (register_on && role && strcmp(role, "assistant") == 0)
      dstr_appendf(d, "%s/%s: ", role, fold_register_label(fold_register_parse(txt)));
   else
      dstr_appendf(d, "%s: ", role ? role : "?");
}

/* Format-aware skeleton emitter. A single message can carry MULTIPLE shapes
 * depending on provider — e.g. an OpenAI assistant turn has BOTH a string
 * `content` AND a separate top-level `tool_calls` array — so the extractors run
 * additively (no early return) and the Coordinate Closet captures identifiers from
 * EVERY provider's tool calls/results, not just Anthropic's. */
static void skeleton_message(dstr_t *d, const cJSON *m, int turn, int excerpt, int register_on,
                             coord_set_t *set)
{
   const char *role = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)m, "role"));
   cJSON *content = cJSON_GetObjectItem((cJSON *)m, "content");
   coord_provenance_t prov = {COORD_LANE_AGENT, turn, -1, -1};

   /* (1) String content (OpenAI/Gemini text, or any plain turn). Emit it but DO
    * NOT return: an OpenAI assistant turn also carries a top-level tool_calls. */
   if (cJSON_IsString(content))
   {
      const char *txt = content->valuestring;
      if (txt)
      {
         coord_closet_nominate(txt, strlen(txt), &prov, set);
         skeleton_prefix(d, role, txt, register_on);
         append_excerpt(d, txt, excerpt);
         dstr_append_char(d, '\n');
      }
   }
   /* (2) Anthropic content-block array (text / tool_use / tool_result). */
   else if (cJSON_IsArray(content))
   {
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
               skeleton_prefix(d, role, txt, register_on);
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

   /* (3) OpenAI / Gemini-as-openai assistant tool calls: a top-level `tool_calls`
    * array. Each element's function.arguments is a JSON STRING. */
   cJSON *tool_calls = cJSON_GetObjectItem((cJSON *)m, "tool_calls");
   if (cJSON_IsArray(tool_calls))
   {
      cJSON *tc;
      cJSON_ArrayForEach(tc, tool_calls)
      {
         cJSON *fn = cJSON_GetObjectItem(tc, "function");
         const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(fn, "name"));
         const char *args = cJSON_GetStringValue(cJSON_GetObjectItem(fn, "arguments"));
         if (args)
            coord_closet_nominate(args, strlen(args), &prov, set);
         dstr_appendf(d, "  $ %s ", name ? name : "tool");
         append_excerpt(d, args ? args : "", excerpt);
         dstr_append_char(d, '\n');
      }
   }

   /* (4) Responses (chatgpt) top-level items: a function_call or its
    * function_call_output (mirror the tool_use / tool_result branches). */
   const char *itype = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)m, "type"));
   if (itype && strcmp(itype, "function_call") == 0)
   {
      const char *name = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)m, "name"));
      const char *args = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)m, "arguments"));
      if (args)
         coord_closet_nominate(args, strlen(args), &prov, set);
      dstr_appendf(d, "  $ %s ", name ? name : "tool");
      append_excerpt(d, args ? args : "", excerpt);
      dstr_append_char(d, '\n');
   }
   else if (itype && strcmp(itype, "function_call_output") == 0)
   {
      cJSON *o = cJSON_GetObjectItem((cJSON *)m, "output");
      char *owned = NULL;
      const char *ov = NULL;
      if (cJSON_IsString(o))
         ov = o->valuestring;
      else if (o)
      {
         owned = cJSON_PrintUnformatted(o);
         ov = owned;
      }
      if (ov)
      {
         coord_closet_nominate(ov, strlen(ov), &prov, set);
         dstr_append_str(d, "    \xE2\x86\x92 "); /* arrow */
         append_excerpt(d, ov, excerpt);
         dstr_appendf(d, " (%zu bytes)\n", strlen(ov));
      }
      free(owned);
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

/* ---- Boundary-free tool-result body compression (economizer Slice 4) ---- */

/* Compress one tool-result body living at `parent[key]` (a string, or any node we
 * serialize). `cc` carries the resolved shrink policy (threshold/head/tail) for the
 * shared compact_body() core. Returns 1 if the body was replaced (and accumulates
 * its ORIGINAL byte length into *raw for the closet ratio cap), 0 if it was below
 * threshold, would not net-shrink, or on OOM. Identifiers in the FULL body are
 * nominated to `set` ONLY when we commit, so a body left full never pollutes the
 * closet. Deterministic. */
static int compress_body_field(cJSON *parent, const char *key, const compact_config_t *cc, int turn,
                               coord_set_t *set, size_t *raw)
{
   cJSON *node = cJSON_GetObjectItem(parent, key);
   char *owned = NULL;
   const char *body = NULL;
   if (cJSON_IsString(node))
      body = node->valuestring;
   else if (node)
   {
      owned = cJSON_PrintUnformatted(node);
      body = owned;
   }
   if (!body)
   {
      free(owned);
      return 0;
   }
   size_t blen = strlen(body);
   if (cc->threshold > 0 && blen <= (size_t)cc->threshold) /* below threshold -> keep verbatim */
   {
      free(owned);
      return 0;
   }

   /* Shrink via the shared core (JSON summary or head+tail), then commit only on a
    * genuine net shrink so an over-threshold-but-tiny body never EXPANDS. The buffer
    * is sized per the compact_body contract so no strategy is truncated. */
   size_t cap = blen + COMPACT_JSON_SUMMARY_MAX + 1;
   char *buf = malloc(cap);
   if (!buf)
   {
      free(owned);
      return 0;
   }
   size_t n = compact_body(body, blen, NULL, cc, buf, cap);
   if (n == 0 || n >= blen)
   {
      free(buf);
      free(owned);
      return 0;
   }

   cJSON *repl = cJSON_CreateString(buf);
   free(buf);
   if (!repl)
   {
      free(owned);
      return 0;
   }
   coord_provenance_t prov = {COORD_LANE_AGENT, turn, -1, -1};
   coord_closet_nominate(body, blen, &prov, set);
   if (!cJSON_ReplaceItemInObjectCaseSensitive(parent, key, repl))
   {
      cJSON_Delete(repl);
      free(owned);
      return 0;
   }
   *raw += blen;
   free(owned);
   return 1;
}

/* Compress every oversized tool-result body carried by one message, across all
 * three provider shapes. Returns the number of bodies compressed in this message.
 * The shapes are mutually exclusive per message (an OpenAI tool result is a string
 * `content`; an Anthropic tool_result is a block inside an ARRAY `content`; a
 * Responses output is a top-level `output`), so no body is double-counted. */
static int compress_message_bodies(cJSON *m, const compact_config_t *cc, int turn, coord_set_t *set,
                                   size_t *raw)
{
   int n = 0;
   const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(m, "role"));
   const char *itype = cJSON_GetStringValue(cJSON_GetObjectItem(m, "type"));

   /* (A) OpenAI / Gemini-as-openai tool result: role=="tool" with string content. */
   if (role && strcmp(role, "tool") == 0)
      n += compress_body_field(m, "content", cc, turn, set, raw);

   /* (B) Anthropic tool_result content-block(s) inside a content ARRAY. (A role
    * "tool" message has a STRING content, so this branch never re-touches it.) */
   cJSON *content = cJSON_GetObjectItem(m, "content");
   if (cJSON_IsArray(content))
   {
      cJSON *b;
      cJSON_ArrayForEach(b, content)
      {
         const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(b, "type"));
         if (t && strcmp(t, "tool_result") == 0)
            n += compress_body_field(b, "content", cc, turn, set, raw);
      }
   }

   /* (C) Responses (chatgpt) top-level function_call_output item: an `output` body. */
   if (itype && strcmp(itype, "function_call_output") == 0)
      n += compress_body_field(m, "output", cc, turn, set, raw);

   return n;
}

int context_compress_view(const cJSON *messages, const fold_config_t *cfg, fold_result_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   out->closet_evict = COORD_EVICT_NONE;
   if (!messages || !cJSON_IsArray((cJSON *)messages) || !cfg || !cfg->enabled)
      return 0;

   int count = cJSON_GetArraySize((cJSON *)messages);
   int retained = cfg->retained_msgs > 0 ? cfg->retained_msgs : CONTEXT_FOLD_DEFAULT_RETAINED_MSGS;
   int keep = cfg->reasoning_excerpt_bytes > 0 ? cfg->reasoning_excerpt_bytes
                                               : CONTEXT_FOLD_DEFAULT_EXCERPT_BYTES;
   if (count <= 0 || retained >= count)
      return 0;                  /* nothing ahead of the retained tail */
   int limit = count - retained; /* messages [0, limit) are compression-eligible */

   /* Resolve the shared shrink policy ONCE. `compact.*` knobs (mirrored into the
    * fold config) are the single source of truth for head/tail, so this seam and the
    * eager seam shrink identically. `keep` (the excerpt budget) is the compression
    * threshold; the tail is capped to keep/2 so a small excerpt budget keeps the
    * tail proportional instead of inheriting a large compact default (§2.4). */
   compact_config_t cc;
   memset(&cc, 0, sizeof(cc));
   cc.enabled = 1;
   cc.threshold = keep;
   cc.head_bytes = cfg->compact_head_bytes; /* 0 -> compact.c default */
   int tail_default =
       cfg->compact_tail_bytes > 0 ? cfg->compact_tail_bytes : COMPACT_DEFAULT_TAIL_BYTES;
   int tail_cap = keep / 2;
   cc.tail_bytes = tail_cap > 0 && tail_cap < tail_default ? tail_cap : tail_default;

   /* Deep-copy the whole transcript, then shrink only oversized tool-result BODIES
    * in the eligible prefix in place. Every message slot, role/type, tool_use_id /
    * tool_call_id, and the ordering survive untouched, so the call/result pairing
    * is byte-for-byte intact (zero orphans for message_history_repair). */
   cJSON *arr = cJSON_Duplicate((cJSON *)messages, 1);
   if (!arr)
      return 0; /* OOM: caller uses the original transcript */

   coord_set_t set;
   coord_set_init(&set);
   size_t compressed_raw = 0; /* total ORIGINAL bytes of compressed bodies (ratio-cap basis) */
   int compressed = 0;
   for (int i = 0; i < limit; i++)
   {
      cJSON *m = cJSON_GetArrayItem(arr, i);
      if (m)
         compressed += compress_message_bodies(m, &cc, i, &set, &compressed_raw);
   }

   if (compressed == 0)
   {
      coord_set_free(&set); /* no body exceeded the threshold -> caller uses original */
      cJSON_Delete(arr);
      return 0;
   }

   /* Conserve any amputated identifiers in a Coordinate Closet, prepended as a
    * synthetic user+assistant note pair (matching context_fold_view): a plain-text
    * turn is valid input for every provider builder, and the PAIR keeps Anthropic
    * role-alternation intact ahead of the (unchanged) original first turn. When the
    * closet is disabled or empty, render returns NULL and we emit no note — the head
    * excerpts still carry the leading bytes of each body. */
   char *closet = coord_closet_render(&set, &cfg->closet, compressed_raw, &out->closet_evict);
   coord_set_free(&set);
   if (closet)
   {
      cJSON *note = cJSON_CreateObject();
      cJSON *ack = cJSON_CreateObject();
      if (note && ack)
      {
         dstr_t body;
         dstr_init(&body);
         dstr_appendf(&body,
                      "[compressed %d oversized tool-result body(ies) above; full bodies remain "
                      "in history — exact identifiers are conserved in the Coordinate Closet]\n\n",
                      compressed);
         dstr_append_str(&body, closet);
         cJSON_AddStringToObject(note, "role", "user");
         cJSON_AddStringToObject(note, "content", dstr_cstr(&body));
         cJSON_AddStringToObject(ack, "role", "assistant");
         cJSON_AddStringToObject(
             ack, "content",
             "Understood — identifiers from the compressed tool results are conserved above.");
         dstr_free(&body);
         /* insert ack first, then note before it, yielding [note, ack, ...original] */
         cJSON_InsertItemInArray(arr, 0, ack);
         cJSON_InsertItemInArray(arr, 0, note);
      }
      else
      {
         cJSON_Delete(note);
         cJSON_Delete(ack);
      }
      free(closet);
   }

   out->messages = arr;
   out->folded = 1; /* flag reused to mean "compressed" */
   out->folded_msgs = compressed;
   out->retained_msgs = retained;
   return 0;
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
      skeleton_message(&body, cJSON_GetArrayItem((cJSON *)messages, i), i, excerpt,
                       cfg->register_enabled, &set);

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
