/* aimee_ir_stream.c -- see aimee_ir_stream.h. */
#include <aimee/translation/aimee_ir_stream.h>

#include <aimee/translation/aimee_backend.h> /* converse_stop_reason (shared with the non-stream parse) */
#include "cJSON.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *ostr(const cJSON *o, const char *k)
{
   const cJSON *it = cJSON_GetObjectItemCaseSensitive((cJSON *)o, k);
   return (it && cJSON_IsString(it)) ? it->valuestring : NULL;
}

static aimee_stop_reason_t finish_to_stop(const char *f)
{
   if (!f)
      return AIMEE_STOP_END_TURN;
   if (strcmp(f, "tool_calls") == 0)
      return AIMEE_STOP_TOOL_USE;
   if (strcmp(f, "length") == 0)
      return AIMEE_STOP_MAX_TOKENS;
   if (strcmp(f, "content_filter") == 0)
      return AIMEE_STOP_CONTENT_FILTER;
   return AIMEE_STOP_END_TURN;
}

void openai_stream_state_init(openai_stream_state_t *st)
{
   if (!st)
      return;
   memset(st, 0, sizeof *st);
   st->text_block = -1;
   st->reasoning_block = -1;
   for (int i = 0; i < AIMEE_STREAM_MAX_TOOLS; i++)
      st->tool_block[i] = -1;
}

int openai_chunk_to_deltas(const cJSON *chunk, openai_stream_state_t *st, aimee_delta_t *out,
                           int max)
{
   int n = 0;
   if (!chunk || !st || !out || max <= 0)
      return 0;
   const cJSON *choices = cJSON_GetObjectItemCaseSensitive((cJSON *)chunk, "choices");
   const cJSON *choice =
       (choices && cJSON_IsArray(choices)) ? cJSON_GetArrayItem((cJSON *)choices, 0) : NULL;
   /* a final usage-only chunk may have no choices; still allow finish/usage below */
   const cJSON *delta = choice ? cJSON_GetObjectItemCaseSensitive((cJSON *)choice, "delta") : NULL;
   const char *finish = choice ? ostr(choice, "finish_reason") : NULL;

#define SLOT() (n < max ? (memset(&out[n], 0, sizeof out[n]), &out[n++]) : NULL)

   if (!st->started)
   {
      aimee_delta_t *d = SLOT();
      if (d)
      {
         d->type = AIMEE_DELTA_TURN_START;
         st->started = 1;
      }
   }

   /* Reasoning first: these providers stream the whole reasoning block before any
    * content, and emitting it as its own THINKING block (rather than folding it into
    * the text block) is what lets a consumer tell thought from answer. Two spellings
    * are in the wild -- `reasoning_content` (DeepSeek, vLLM, llama.cpp) and
    * `reasoning` (OpenRouter); OpenAI's own API sends neither. */
   const char *reasoning = delta ? ostr(delta, "reasoning_content") : NULL;
   if (!reasoning && delta)
      reasoning = ostr(delta, "reasoning");
   if (reasoning && reasoning[0])
   {
      if (st->reasoning_block < 0)
      {
         aimee_delta_t *d = SLOT();
         if (d)
         {
            d->type = AIMEE_DELTA_BLOCK_START;
            d->kind = AIMEE_BLK_THINKING;
            d->block_id = st->next_block;
            st->reasoning_block = st->next_block++;
         }
      }
      aimee_delta_t *d = SLOT();
      if (d)
      {
         d->type = AIMEE_DELTA_BLOCK_DELTA;
         d->kind = AIMEE_BLK_THINKING;
         d->block_id = st->reasoning_block;
         d->text_delta = reasoning;
      }
   }

   const char *content = delta ? ostr(delta, "content") : NULL;
   const cJSON *tcs = delta ? cJSON_GetObjectItemCaseSensitive((cJSON *)delta, "tool_calls") : NULL;

   /* Reasoning is over once content or a tool call starts: close the block there
    * rather than deferring to `finish`, so a consumer can tell "still thinking" from
    * "answering". A later reasoning delta just opens a fresh THINKING block. */
   if (st->reasoning_block >= 0 && ((content && content[0]) || (tcs && cJSON_IsArray(tcs))))
   {
      aimee_delta_t *d = SLOT();
      if (d)
      {
         d->type = AIMEE_DELTA_BLOCK_STOP;
         d->kind = AIMEE_BLK_THINKING;
         d->block_id = st->reasoning_block;
         st->reasoning_block = -1;
      }
   }

   if (content && content[0])
   {
      if (st->text_block < 0)
      {
         aimee_delta_t *d = SLOT();
         if (d)
         {
            d->type = AIMEE_DELTA_BLOCK_START;
            d->kind = AIMEE_BLK_TEXT;
            d->block_id = st->next_block;
            st->text_block = st->next_block++;
         }
      }
      aimee_delta_t *d = SLOT();
      if (d)
      {
         d->type = AIMEE_DELTA_BLOCK_DELTA;
         d->kind = AIMEE_BLK_TEXT;
         d->block_id = st->text_block;
         d->text_delta = content;
      }
   }

   if (tcs && cJSON_IsArray(tcs))
   {
      const cJSON *tc = NULL;
      cJSON_ArrayForEach(tc, tcs)
      {
         const cJSON *jidx = cJSON_GetObjectItemCaseSensitive((cJSON *)tc, "index");
         int idx = (jidx && cJSON_IsNumber(jidx)) ? jidx->valueint : 0;
         if (idx < 0 || idx >= AIMEE_STREAM_MAX_TOOLS)
            continue;
         const cJSON *fn = cJSON_GetObjectItemCaseSensitive((cJSON *)tc, "function");
         if (st->tool_block[idx] < 0)
         {
            aimee_delta_t *d = SLOT();
            if (d)
            {
               d->type = AIMEE_DELTA_BLOCK_START;
               d->kind = AIMEE_BLK_TOOL_USE;
               d->block_id = st->next_block;
               d->tool_id = ostr(tc, "id");
               d->tool_name = fn ? ostr(fn, "name") : NULL;
               st->tool_block[idx] = st->next_block++;
            }
         }
         const char *args = fn ? ostr(fn, "arguments") : NULL;
         if (args && args[0])
         {
            aimee_delta_t *d = SLOT();
            if (d)
            {
               d->type = AIMEE_DELTA_BLOCK_DELTA;
               d->kind = AIMEE_BLK_TOOL_USE;
               d->block_id = st->tool_block[idx];
               d->tool_args_delta = args;
            }
         }
      }
   }

   if (finish && !st->stopped)
   {
      if (st->reasoning_block >= 0)
      {
         aimee_delta_t *d = SLOT();
         if (d)
         {
            d->type = AIMEE_DELTA_BLOCK_STOP;
            d->kind = AIMEE_BLK_THINKING;
            d->block_id = st->reasoning_block;
            st->reasoning_block = -1;
         }
      }
      if (st->text_block >= 0)
      {
         aimee_delta_t *d = SLOT();
         if (d)
         {
            d->type = AIMEE_DELTA_BLOCK_STOP;
            d->block_id = st->text_block;
         }
      }
      for (int i = 0; i < AIMEE_STREAM_MAX_TOOLS; i++)
      {
         if (st->tool_block[i] < 0)
            continue;
         aimee_delta_t *d = SLOT();
         if (d)
         {
            d->type = AIMEE_DELTA_BLOCK_STOP;
            d->block_id = st->tool_block[i];
         }
      }
      aimee_delta_t *d = SLOT();
      if (d)
      {
         d->type = AIMEE_DELTA_TURN_STOP;
         d->stop_reason = finish_to_stop(finish);
         const cJSON *usage = cJSON_GetObjectItemCaseSensitive((cJSON *)chunk, "usage");
         if (usage)
         {
            const cJSON *pt = cJSON_GetObjectItemCaseSensitive((cJSON *)usage, "prompt_tokens");
            const cJSON *ct = cJSON_GetObjectItemCaseSensitive((cJSON *)usage, "completion_tokens");
            if (pt && cJSON_IsNumber(pt))
               d->usage_in = (long)pt->valuedouble;
            if (ct && cJSON_IsNumber(ct))
               d->usage_out = (long)ct->valuedouble;
         }
         st->stopped = 1;
      }
   }
#undef SLOT
   return n;
}

void converse_stream_state_init(converse_stream_state_t *st)
{
   if (!st)
      return;
   memset(st, 0, sizeof *st);
}

static int converse_index(const cJSON *payload, int *out)
{
   const cJSON *it =
       payload ? cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "contentBlockIndex") : NULL;
   if (!cJSON_IsNumber(it) || !isfinite(it->valuedouble) ||
       floor(it->valuedouble) != it->valuedouble || it->valuedouble < 0 ||
       it->valuedouble >= AIMEE_STREAM_MAX_TOOLS)
      return -1;
   *out = (int)it->valuedouble;
   return 0;
}

static int converse_usage(const cJSON *usage, const char *name, long *out)
{
   const cJSON *it = usage ? cJSON_GetObjectItemCaseSensitive((cJSON *)usage, name) : NULL;
   if (!cJSON_IsNumber(it) || !isfinite(it->valuedouble) || it->valuedouble < 0 ||
       it->valuedouble > 9007199254740991.0 || it->valuedouble > LONG_MAX ||
       floor(it->valuedouble) != it->valuedouble)
      return -1;
   *out = (long)it->valuedouble;
   return 0;
}

static int converse_optional_usage(const cJSON *usage, const char *name)
{
   if (!cJSON_GetObjectItemCaseSensitive((cJSON *)usage, name))
      return 0;
   long ignored;
   return converse_usage(usage, name, &ignored);
}

/* Is `ev` one of the known ConverseStream exception event-types? */
static int converse_is_exception(const char *ev)
{
   return strcmp(ev, "internalServerException") == 0 ||
          strcmp(ev, "modelStreamErrorException") == 0 || strcmp(ev, "validationException") == 0 ||
          strcmp(ev, "throttlingException") == 0 || strcmp(ev, "serviceUnavailableException") == 0;
}

int bedrock_converse_stream_to_deltas(const char *event_type, const cJSON *payload,
                                      converse_stream_state_t *st, aimee_delta_t *out, int max)
{
   if (!event_type || !st || !out || max <= 0)
      return 0;

   if (strcmp(event_type, "messageStart") == 0)
   {
      if (!payload || !ostr(payload, "role") || strcmp(ostr(payload, "role"), "assistant") != 0)
         return -1;
      memset(&out[0], 0, sizeof out[0]);
      out[0].type = AIMEE_DELTA_TURN_START;
      return 1;
   }

   if (strcmp(event_type, "contentBlockStart") == 0)
   {
      if (!payload)
         return -1;
      int idx;
      if (converse_index(payload, &idx) != 0)
         return -1; /* a valid Converse stream never has an out-of-range index */
      memset(&out[0], 0, sizeof out[0]);
      out[0].type = AIMEE_DELTA_BLOCK_START;
      out[0].block_id = idx;
      const cJSON *start = cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "start");
      const cJSON *tu = start ? cJSON_GetObjectItemCaseSensitive((cJSON *)start, "toolUse") : NULL;
      if (!cJSON_IsObject(start) || !cJSON_IsObject(tu) || !ostr(tu, "toolUseId") ||
          !ostr(tu, "name") || cJSON_GetArraySize(start) != 1 || st->kind_set[idx])
         return -1;
      aimee_block_type_t kind = AIMEE_BLK_TOOL_USE;
      out[0].tool_id = ostr(tu, "toolUseId");
      out[0].tool_name = ostr(tu, "name");
      out[0].kind = kind;
      if (idx >= 0 && idx < AIMEE_STREAM_MAX_TOOLS)
      {
         st->kind[idx] = kind;
         st->kind_set[idx] = 1;
      }
      return 1;
   }

   if (strcmp(event_type, "contentBlockDelta") == 0)
   {
      if (!payload)
         return -1;
      int idx;
      if (converse_index(payload, &idx) != 0)
         return -1;
      const cJSON *delta = cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "delta");
      if (!delta || !cJSON_IsObject(delta))
         return -1; /* structurally-malformed KNOWN event -> drop the stream */
      const char *text = ostr(delta, "text");
      const cJSON *tu = cJSON_GetObjectItemCaseSensitive((cJSON *)delta, "toolUse");
      const char *tuin = tu ? ostr(tu, "input") : NULL;
      const cJSON *rc = cJSON_GetObjectItemCaseSensitive((cJSON *)delta, "reasoningContent");
      const char *rtext = rc ? ostr(rc, "text") : NULL;
      if (!!text + !!tuin + !!rtext != 1)
         return -1;
      aimee_block_type_t kind;
      /* kind self-identifies from the delta's own union variant. */
      if (text)
      {
         kind = AIMEE_BLK_TEXT;
      }
      else if (tuin)
      {
         /* toolUse.input is a JSON-STRING fragment accumulated across deltas -- emit
          * it verbatim, do NOT parse. */
         kind = AIMEE_BLK_TOOL_USE;
      }
      else
      {
         kind = AIMEE_BLK_THINKING;
      }
      if (st->kind_set[idx] && st->kind[idx] != kind)
         return -1;
      if (!st->kind_set[idx] && kind == AIMEE_BLK_TOOL_USE)
         return -1;
      int first = !st->kind_set[idx];
      if (first && max < 2)
         return -1;
      int delta_slot = first ? 1 : 0;
      if (first)
      {
         memset(&out[0], 0, sizeof out[0]);
         out[0].type = AIMEE_DELTA_BLOCK_START;
         out[0].block_id = idx;
         out[0].kind = kind;
         st->kind[idx] = kind;
         st->kind_set[idx] = 1;
      }
      memset(&out[delta_slot], 0, sizeof out[delta_slot]);
      out[delta_slot].type = AIMEE_DELTA_BLOCK_DELTA;
      out[delta_slot].block_id = idx;
      out[delta_slot].kind = kind;
      if (kind == AIMEE_BLK_TOOL_USE)
         out[delta_slot].tool_args_delta = tuin;
      else
         out[delta_slot].text_delta = text ? text : rtext;
      return first ? 2 : 1;
   }

   if (strcmp(event_type, "contentBlockStop") == 0)
   {
      if (!payload)
         return -1;
      int idx;
      if (converse_index(payload, &idx) != 0)
         return -1;
      if (!st->kind_set[idx])
         return -1;
      memset(&out[0], 0, sizeof out[0]);
      out[0].type = AIMEE_DELTA_BLOCK_STOP;
      out[0].block_id = idx;
      out[0].kind = st->kind[idx];
      return 1;
   }

   if (strcmp(event_type, "messageStop") == 0)
   {
      if (!payload || !ostr(payload, "stopReason") || st->message_stop_seen || st->terminal_emitted)
         return -1;
      st->pending_stop_reason = converse_stop_reason(ostr(payload, "stopReason"));
      st->message_stop_seen = 1;
      return 0; /* metadata supplies usage for the single terminal IR delta */
   }

   if (strcmp(event_type, "metadata") == 0)
   {
      if (!st->message_stop_seen || st->terminal_emitted)
         return -1;
      aimee_delta_t terminal = {.type = AIMEE_DELTA_TURN_STOP,
                                .stop_reason = st->pending_stop_reason};
      const cJSON *usage =
          payload ? cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "usage") : NULL;
      if (!cJSON_IsObject(usage) || converse_usage(usage, "inputTokens", &terminal.usage_in) != 0 ||
          converse_usage(usage, "outputTokens", &terminal.usage_out) != 0 ||
          converse_optional_usage(usage, "cacheReadInputTokens") != 0 ||
          converse_optional_usage(usage, "cacheWriteInputTokens") != 0)
         return -1;
      out[0] = terminal;
      st->terminal_emitted = 1;
      /* aimee_delta_t has no cache fields -> cache tokens dropped on the stream path. */
      return 1;
   }

   if (converse_is_exception(event_type))
   {
      memset(&out[0], 0, sizeof out[0]);
      out[0].type = AIMEE_DELTA_ERROR;
      const char *msg = payload ? ostr(payload, "message") : NULL;
      out[0].error_message = msg ? msg : event_type;
      return 1;
   }

   /* a genuinely-unknown event_type (a future Converse event) -> 0 deltas. */
   return 0;
}

/* Read a usage counter LENIENTLY -- matching anthropic_backend_parse's treatment of
 * the same fields on the non-stream path (absent/garbage leaves the counter
 * untouched) rather than the Converse parser's strict validation, so the two
 * Anthropic parsers agree about what a usage block means. */
static void anthropic_usage(const cJSON *usage, const char *name, long *out)
{
   const cJSON *it = usage ? cJSON_GetObjectItemCaseSensitive((cJSON *)usage, name) : NULL;
   if (cJSON_IsNumber(it) && isfinite(it->valuedouble) && it->valuedouble >= 0 &&
       it->valuedouble <= (double)LONG_MAX)
      *out = (long)it->valuedouble;
}

/* Anthropic content_block.type -> IR block kind. An unrecognised type maps to
 * AIMEE_BLK_UNKNOWN rather than dropping the stream, so a future block type still
 * gets well-formed START/STOP bracketing around deltas we ignore. */
static aimee_block_type_t anthropic_block_kind(const char *type)
{
   if (!type)
      return AIMEE_BLK_UNKNOWN;
   if (strcmp(type, "text") == 0)
      return AIMEE_BLK_TEXT;
   /* redacted_thinking carries no plaintext, but it IS a reasoning block -- keeping it
    * THINKING means a consumer counting reasoning is not fooled by redaction. */
   if (strcmp(type, "thinking") == 0 || strcmp(type, "redacted_thinking") == 0)
      return AIMEE_BLK_THINKING;
   if (strcmp(type, "tool_use") == 0)
      return AIMEE_BLK_TOOL_USE;
   return AIMEE_BLK_UNKNOWN;
}

/* content-block index -> block_id, bounded by the per-index kind table. */
static int anthropic_index(const cJSON *payload, int *out)
{
   const cJSON *it = cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "index");
   if (!cJSON_IsNumber(it) || !isfinite(it->valuedouble) || it->valuedouble < 0 ||
       it->valuedouble >= AIMEE_STREAM_MAX_TOOLS)
      return -1;
   /* Integrality without floor(): the range check above makes the int cast safe, and
    * avoiding libm here keeps every caller of this parser free of a -lm dependency. */
   if ((double)(int)it->valuedouble != it->valuedouble)
      return -1;
   *out = (int)it->valuedouble;
   return 0;
}

void anthropic_backend_stream_state_init(anthropic_backend_stream_state_t *st)
{
   if (!st)
      return;
   memset(st, 0, sizeof *st);
   /* Absent a stop_reason (message_stop with no preceding final message_delta), fall
    * back to end_turn -- the same default openai's finish_to_stop applies to NULL. */
   st->pending_stop_reason = AIMEE_STOP_END_TURN;
}

int anthropic_stream_to_deltas(const char *event_type, const cJSON *payload,
                               anthropic_backend_stream_state_t *st, aimee_delta_t *out, int max)
{
   if (!event_type || !st || !out || max <= 0)
      return 0;

   if (strcmp(event_type, "ping") == 0)
      return 0;

   if (strcmp(event_type, "message_start") == 0)
   {
      const cJSON *msg =
          payload ? cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "message") : NULL;
      const char *role = msg ? ostr(msg, "role") : NULL;
      if (!cJSON_IsObject(msg) || !role || strcmp(role, "assistant") != 0 || st->started ||
          st->terminal_emitted)
         return -1;
      anthropic_usage(cJSON_GetObjectItemCaseSensitive((cJSON *)msg, "usage"), "input_tokens",
                      &st->pending_usage_in);
      memset(&out[0], 0, sizeof out[0]);
      out[0].type = AIMEE_DELTA_TURN_START;
      st->started = 1;
      return 1;
   }

   if (strcmp(event_type, "content_block_start") == 0)
   {
      int idx;
      if (!payload || anthropic_index(payload, &idx) != 0)
         return -1;
      const cJSON *cb = cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "content_block");
      if (!cJSON_IsObject(cb) || st->kind_set[idx])
         return -1;
      aimee_block_type_t kind = anthropic_block_kind(ostr(cb, "type"));
      const char *tool_id = (kind == AIMEE_BLK_TOOL_USE) ? ostr(cb, "id") : NULL;
      const char *tool_name = (kind == AIMEE_BLK_TOOL_USE) ? ostr(cb, "name") : NULL;
      if (kind == AIMEE_BLK_TOOL_USE && (!tool_id || !tool_name))
         return -1; /* a tool_use block with no id/name cannot be answered */
      memset(&out[0], 0, sizeof out[0]);
      out[0].type = AIMEE_DELTA_BLOCK_START;
      out[0].block_id = idx;
      out[0].kind = kind;
      out[0].tool_id = tool_id;
      out[0].tool_name = tool_name;
      st->kind[idx] = kind;
      st->kind_set[idx] = 1;
      return 1;
   }

   if (strcmp(event_type, "content_block_delta") == 0)
   {
      int idx;
      if (!payload || anthropic_index(payload, &idx) != 0)
         return -1;
      const cJSON *delta = cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "delta");
      if (!cJSON_IsObject(delta) || !st->kind_set[idx])
         return -1; /* Anthropic always brackets deltas with a content_block_start */
      const char *dtype = ostr(delta, "type");
      if (!dtype)
         return -1;

      /* signature_delta: see the header's KNOWN GAPS. The IR delta model has nowhere
       * to put a signature, so it is dropped openly rather than mangled into text. */
      if (strcmp(dtype, "signature_delta") == 0)
         return 0;

      aimee_block_type_t kind;
      const char *text = NULL, *args = NULL;
      if (strcmp(dtype, "text_delta") == 0)
      {
         kind = AIMEE_BLK_TEXT;
         text = ostr(delta, "text");
      }
      else if (strcmp(dtype, "thinking_delta") == 0)
      {
         kind = AIMEE_BLK_THINKING;
         text = ostr(delta, "thinking");
      }
      else if (strcmp(dtype, "input_json_delta") == 0)
      {
         /* partial_json is a JSON-STRING fragment accumulated across deltas -- emit it
          * verbatim, do NOT parse (same contract as Converse's toolUse.input). */
         kind = AIMEE_BLK_TOOL_USE;
         args = ostr(delta, "partial_json");
      }
      else
         return 0; /* a future delta variant -> ignore it, keep the stream */

      if (!text && !args)
         return -1; /* a KNOWN variant missing its own payload field */
      if (st->kind[idx] != kind)
         return -1;

      memset(&out[0], 0, sizeof out[0]);
      out[0].type = AIMEE_DELTA_BLOCK_DELTA;
      out[0].block_id = idx;
      out[0].kind = kind;
      if (args)
         out[0].tool_args_delta = args;
      else
         out[0].text_delta = text;
      return 1;
   }

   if (strcmp(event_type, "content_block_stop") == 0)
   {
      int idx;
      if (!payload || anthropic_index(payload, &idx) != 0 || !st->kind_set[idx])
         return -1;
      memset(&out[0], 0, sizeof out[0]);
      out[0].type = AIMEE_DELTA_BLOCK_STOP;
      out[0].block_id = idx;
      out[0].kind = st->kind[idx];
      return 1;
   }

   if (strcmp(event_type, "message_delta") == 0)
   {
      if (!payload || !st->started || st->terminal_emitted)
         return -1;
      const cJSON *delta = cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "delta");
      if (!cJSON_IsObject(delta))
         return -1;
      /* stop_reason is null on a non-final message_delta; only the last one sets it. */
      const char *sr = ostr(delta, "stop_reason");
      if (sr)
         st->pending_stop_reason = aimee_stop_reason_parse(sr);
      anthropic_usage(cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "usage"), "output_tokens",
                      &st->pending_usage_out);
      return 0; /* message_stop emits the single terminal IR delta */
   }

   if (strcmp(event_type, "message_stop") == 0)
   {
      if (!st->started || st->terminal_emitted)
         return -1;
      memset(&out[0], 0, sizeof out[0]);
      out[0].type = AIMEE_DELTA_TURN_STOP;
      out[0].stop_reason = st->pending_stop_reason;
      out[0].usage_in = st->pending_usage_in;
      out[0].usage_out = st->pending_usage_out;
      st->terminal_emitted = 1;
      return 1;
   }

   if (strcmp(event_type, "error") == 0)
   {
      const cJSON *e = payload ? cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "error") : NULL;
      const char *msg = e ? ostr(e, "message") : NULL;
      memset(&out[0], 0, sizeof out[0]);
      out[0].type = AIMEE_DELTA_ERROR;
      out[0].error_message = msg ? msg : "anthropic stream error";
      return 1;
   }

   /* a genuinely-unknown event_type (a future Anthropic event) -> 0 deltas. */
   return 0;
}

/* wrap a cJSON `data` object as an SSE frame "event: <ev>\ndata: <json>\n\n"
 * (consumes data). */
static char *sse_frame(const char *ev, cJSON *data)
{
   char *json = cJSON_PrintUnformatted(data);
   cJSON_Delete(data);
   if (!json)
      return NULL;
   size_t need =
       strlen("event: ") + strlen(ev) + strlen("\ndata: ") + strlen(json) + strlen("\n\n") + 1;
   char *buf = malloc(need);
   if (buf)
      snprintf(buf, need, "event: %s\ndata: %s\n\n", ev, json);
   free(json);
   return buf;
}

/* Build the Anthropic SSE event object(s) for one IR delta into ev[]/js[] (up to
 * 2 -- TURN_STOP is message_delta + message_stop). Returns the count (0 = no
 * output). The caller owns the returned cJSON (frees / serializes them). st is
 * updated (TURN_START sets st->started). Shared by anthropic_delta_render (frames)
 * and anthropic_delta_emit (callback) so the two never drift. */
static int delta_build_events(const aimee_delta_t *d, anthropic_stream_state_t *st,
                              const char *msg_id, const char *model, const char *ev[2],
                              cJSON *js[2])
{
   if (!d)
      return 0;
   switch (d->type)
   {
   case AIMEE_DELTA_TURN_START:
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "type", "message_start");
      cJSON *m = cJSON_AddObjectToObject(o, "message");
      cJSON_AddStringToObject(m, "id", msg_id ? msg_id : "");
      cJSON_AddStringToObject(m, "type", "message");
      cJSON_AddStringToObject(m, "role", "assistant");
      if (model)
         cJSON_AddStringToObject(m, "model", model);
      cJSON *u = cJSON_AddObjectToObject(m, "usage");
      cJSON_AddNumberToObject(u, "input_tokens", 0);
      if (st)
         st->started = 1;
      ev[0] = "message_start";
      js[0] = o;
      return 1;
   }
   case AIMEE_DELTA_BLOCK_START:
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "type", "content_block_start");
      cJSON_AddNumberToObject(o, "index", d->block_id);
      cJSON *cb = cJSON_AddObjectToObject(o, "content_block");
      if (d->kind == AIMEE_BLK_TOOL_USE)
      {
         cJSON_AddStringToObject(cb, "type", "tool_use");
         cJSON_AddStringToObject(cb, "id", d->tool_id ? d->tool_id : "");
         cJSON_AddStringToObject(cb, "name", d->tool_name ? d->tool_name : "");
         cJSON_AddItemToObject(cb, "input", cJSON_CreateObject());
      }
      else
      {
         cJSON_AddStringToObject(cb, "type", "text");
         cJSON_AddStringToObject(cb, "text", "");
      }
      ev[0] = "content_block_start";
      js[0] = o;
      return 1;
   }
   case AIMEE_DELTA_BLOCK_DELTA:
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "type", "content_block_delta");
      cJSON_AddNumberToObject(o, "index", d->block_id);
      cJSON *dl = cJSON_AddObjectToObject(o, "delta");
      if (d->kind == AIMEE_BLK_TOOL_USE)
      {
         cJSON_AddStringToObject(dl, "type", "input_json_delta");
         cJSON_AddStringToObject(dl, "partial_json", d->tool_args_delta ? d->tool_args_delta : "");
      }
      else
      {
         cJSON_AddStringToObject(dl, "type", "text_delta");
         cJSON_AddStringToObject(dl, "text", d->text_delta ? d->text_delta : "");
      }
      ev[0] = "content_block_delta";
      js[0] = o;
      return 1;
   }
   case AIMEE_DELTA_BLOCK_STOP:
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "type", "content_block_stop");
      cJSON_AddNumberToObject(o, "index", d->block_id);
      ev[0] = "content_block_stop";
      js[0] = o;
      return 1;
   }
   case AIMEE_DELTA_TURN_STOP:
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "type", "message_delta");
      cJSON *dl = cJSON_AddObjectToObject(o, "delta");
      cJSON_AddStringToObject(dl, "stop_reason", aimee_stop_reason_name(d->stop_reason));
      cJSON_AddNullToObject(dl, "stop_sequence");
      cJSON *u = cJSON_AddObjectToObject(o, "usage");
      cJSON_AddNumberToObject(u, "output_tokens", (double)d->usage_out);
      ev[0] = "message_delta";
      js[0] = o;
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "message_stop");
      ev[1] = "message_stop";
      js[1] = s;
      return 2;
   }
   case AIMEE_DELTA_ERROR:
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "type", "error");
      cJSON *e = cJSON_AddObjectToObject(o, "error");
      cJSON_AddStringToObject(e, "type", "api_error");
      cJSON_AddStringToObject(e, "message", d->error_message ? d->error_message : "stream error");
      ev[0] = "error";
      js[0] = o;
      return 1;
   }
   default:
      return 0;
   }
}

char *anthropic_delta_render(const aimee_delta_t *d, anthropic_stream_state_t *st,
                             const char *msg_id, const char *model)
{
   const char *ev[2] = {NULL, NULL};
   cJSON *js[2] = {NULL, NULL};
   int n = delta_build_events(d, st, msg_id, model, ev, js);
   if (n == 0)
      return NULL;
   char *first = sse_frame(ev[0], js[0]); /* sse_frame takes ownership of js[i] */
   if (n == 1)
      return first;
   char *second = sse_frame(ev[1], js[1]);
   if (!first || !second)
   {
      free(first);
      free(second);
      return NULL;
   }
   size_t need = strlen(first) + strlen(second) + 1;
   char *both = malloc(need);
   if (both)
      snprintf(both, need, "%s%s", first, second);
   free(first);
   free(second);
   return both;
}

int anthropic_delta_emit(const aimee_delta_t *d, anthropic_stream_state_t *st, const char *msg_id,
                         const char *model, aimee_sse_emit_fn emit, void *ctx)
{
   const char *ev[2] = {NULL, NULL};
   cJSON *js[2] = {NULL, NULL};
   int n = delta_build_events(d, st, msg_id, model, ev, js);
   for (int i = 0; i < n; i++)
   {
      char *json = cJSON_PrintUnformatted(js[i]);
      cJSON_Delete(js[i]);
      if (json && emit)
         emit(ctx, ev[i], json);
      free(json);
   }
   return n;
}
