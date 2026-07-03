/* aimee_ir_stream.c -- see aimee_ir_stream.h. */
#include "aimee_ir_stream.h"

#include "cJSON.h"

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

   const char *content = delta ? ostr(delta, "content") : NULL;
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

   const cJSON *tcs = delta ? cJSON_GetObjectItemCaseSensitive((cJSON *)delta, "tool_calls") : NULL;
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
