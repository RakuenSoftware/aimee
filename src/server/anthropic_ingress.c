/* anthropic_ingress.c: inbound Anthropic Messages API translation.
 *
 * Pure (cJSON-only) wire-format translation used by the POST /v1/messages
 * ingress so Claude Code can drive aimee's primary model. See
 * anthropic_ingress.h for the contract and the "stateless proxy, not the agent
 * loop" constraint. This is the inverse of the outbound delegate drivers:
 * Anthropic Messages JSON in, OpenAI chat/completions JSON out (and the
 * parsed reply back into Anthropic shape). No DB, network, or agent state. */
#include "anthropic_ingress.h"
#include "cJSON.h"
#include "json_fluent.h"
#include <stdlib.h>
#include <string.h>

/* --- Per-request passthrough headers (parity mode) ---------------------------
 *
 * Not part of the pure translation: a thin per-request echo of the two inbound
 * headers that select Anthropic API behavior. server_http captures them off the
 * raw request (the ingress handlers never see HTTP headers); the anthropic-driver
 * passthrough forwards them upstream when the primary speaks the Anthropic API. Thread-local
 * and reset on every request, so a value never leaks between requests on a reused
 * worker thread. */
static __thread char g_req_anthropic_version[64] = "";
static __thread char g_req_anthropic_beta[512] = "";

void anthropic_ingress_set_request_headers(const char *anthropic_version,
                                           const char *anthropic_beta)
{
   snprintf(g_req_anthropic_version, sizeof(g_req_anthropic_version), "%s",
            anthropic_version ? anthropic_version : "");
   snprintf(g_req_anthropic_beta, sizeof(g_req_anthropic_beta), "%s",
            anthropic_beta ? anthropic_beta : "");
}

const char *anthropic_ingress_request_version(void)
{
   return g_req_anthropic_version;
}

const char *anthropic_ingress_request_beta(void)
{
   return g_req_anthropic_beta;
}

/* Append `add` to the malloc'd accumulator `acc`, inserting `sep` between an
 * existing value and the new one. Returns the (possibly reallocated) buffer;
 * on allocation failure the original buffer is returned unchanged. */
static char *str_append_join(char *acc, const char *add, const char *sep)
{
   size_t la, ls, ld;
   char *n;

   if (!add)
      return acc;
   if (!acc)
      return strdup(add);

   la = strlen(acc);
   ls = sep ? strlen(sep) : 0;
   ld = strlen(add);
   n = realloc(acc, la + ls + ld + 1);
   if (!n)
      return acc;
   if (ls)
      memcpy(n + la, sep, ls);
   memcpy(n + la + ls, add, ld);
   n[la + ls + ld] = '\0';
   return n;
}

char *anthropic_system_to_text(const cJSON *req)
{
   const cJSON *sys = cJSON_GetObjectItemCaseSensitive(req, "system");
   const cJSON *blk;
   char *acc = NULL;

   if (!sys)
      return NULL;
   if (cJSON_IsString(sys))
      return (sys->valuestring && sys->valuestring[0]) ? strdup(sys->valuestring) : NULL;
   if (!cJSON_IsArray(sys))
      return NULL;

   cJSON_ArrayForEach(blk, sys)
   {
      const cJSON *t;
      if (strcmp(jo_cstr(blk, "type"), "text") != 0)
         continue;
      t = cJSON_GetObjectItemCaseSensitive(blk, "text");
      if (cJSON_IsString(t) && t->valuestring[0])
         acc = str_append_join(acc, t->valuestring, acc ? "\n\n" : NULL);
   }
   return acc;
}

/* Build a "data:<media_type>;base64,<data>" URL. Caller frees. */
static char *build_data_url(const char *media_type, const char *data)
{
   const char *mt = (media_type && media_type[0]) ? media_type : "image/png";
   const char *d = data ? data : "";
   size_t n = strlen("data:") + strlen(mt) + strlen(";base64,") + strlen(d) + 1;
   char *url = malloc(n);

   if (!url)
      return NULL;
   snprintf(url, n, "data:%s;base64,%s", mt, d);
   return url;
}

/* Map one Anthropic image block's source into an OpenAI image_url string.
 * Supports source.type "base64" (media_type + data) and "url" (url). */
static char *image_block_to_url(const cJSON *blk)
{
   const cJSON *src = cJSON_GetObjectItemCaseSensitive(blk, "source");
   const char *stype = jo_cstr(src, "type");

   if (strcmp(stype, "url") == 0)
      return strdup(jo_cstr(src, "url"));
   return build_data_url(jo_cstr(src, "media_type"), jo_cstr(src, "data"));
}

/* Flatten a tool_result "content" value (string or array of text/blocks) into
 * a single malloc'd string. Returns "" for empty/array-without-text. */
static char *flatten_block_content(const cJSON *content)
{
   const cJSON *blk;
   char *acc = NULL;

   if (!content)
      return strdup("");
   if (cJSON_IsString(content))
      return strdup(content->valuestring ? content->valuestring : "");
   if (!cJSON_IsArray(content))
      return strdup("");

   cJSON_ArrayForEach(blk, content)
   {
      const cJSON *t;
      if (strcmp(jo_cstr(blk, "type"), "text") != 0)
         continue;
      t = cJSON_GetObjectItemCaseSensitive(blk, "text");
      if (cJSON_IsString(t))
         acc = str_append_join(acc, t->valuestring, acc ? "\n" : NULL);
   }
   return acc ? acc : strdup("");
}

/* Emit one OpenAI {role:"tool", ...} message per tool_result block. OpenAI
 * requires these to immediately follow the assistant message carrying the
 * matching tool_calls, so the caller emits them before any user text. */
static void emit_tool_result_messages(const cJSON *content, cJSON *out)
{
   const cJSON *blk;

   cJSON_ArrayForEach(blk, content)
   {
      cJSON *msg;
      char *text;
      if (strcmp(jo_cstr(blk, "type"), "tool_result") != 0)
         continue;
      msg = cJSON_CreateObject();
      cJSON_AddStringToObject(msg, "role", "tool");
      cJSON_AddStringToObject(msg, "tool_call_id", jo_cstr(blk, "tool_use_id"));
      text = flatten_block_content(cJSON_GetObjectItemCaseSensitive(blk, "content"));
      cJSON_AddStringToObject(msg, "content", text ? text : "");
      free(text);
      cJSON_AddItemToArray(out, msg);
   }
}

/* Emit a single OpenAI user message from the text/image blocks of an Anthropic
 * user turn. tool_result blocks are skipped (handled separately). When images
 * are present the content is an array of typed parts; otherwise a plain
 * string. No message is emitted when there is no text/image content. */
static void emit_user_content_message(const cJSON *content, cJSON *out)
{
   const cJSON *blk;
   int has_image = 0, has_any = 0;
   cJSON *msg;

   cJSON_ArrayForEach(blk, content)
   {
      const char *type = jo_cstr(blk, "type");
      if (strcmp(type, "image") == 0)
         has_image = has_any = 1;
      else if (strcmp(type, "text") == 0)
         has_any = 1;
   }
   if (!has_any)
      return;

   msg = cJSON_CreateObject();
   cJSON_AddStringToObject(msg, "role", "user");

   if (!has_image)
   {
      char *text = NULL;
      cJSON_ArrayForEach(blk, content)
      {
         const cJSON *t;
         if (strcmp(jo_cstr(blk, "type"), "text") != 0)
            continue;
         t = cJSON_GetObjectItemCaseSensitive(blk, "text");
         if (cJSON_IsString(t))
            text = str_append_join(text, t->valuestring, text ? "\n" : NULL);
      }
      cJSON_AddStringToObject(msg, "content", text ? text : "");
      free(text);
   }
   else
   {
      cJSON *parts = cJSON_AddArrayToObject(msg, "content");
      cJSON_ArrayForEach(blk, content)
      {
         const char *type = jo_cstr(blk, "type");
         if (strcmp(type, "text") == 0)
         {
            cJSON *p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "type", "text");
            cJSON_AddStringToObject(p, "text", jo_cstr(blk, "text"));
            cJSON_AddItemToArray(parts, p);
         }
         else if (strcmp(type, "image") == 0)
         {
            char *url = image_block_to_url(blk);
            cJSON *p = cJSON_CreateObject();
            cJSON *iu;
            cJSON_AddStringToObject(p, "type", "image_url");
            iu = cJSON_AddObjectToObject(p, "image_url");
            cJSON_AddStringToObject(iu, "url", url ? url : "");
            free(url);
            cJSON_AddItemToArray(parts, p);
         }
      }
   }
   cJSON_AddItemToArray(out, msg);
}

/* Emit one OpenAI assistant message from an Anthropic assistant turn. Joins
 * text blocks into `content` (null when only tool_use is present) and maps
 * each tool_use block into a tool_calls[] entry with stringified arguments. */
static void emit_assistant_message(const cJSON *content, cJSON *out)
{
   const cJSON *blk;
   char *text = NULL;
   cJSON *tool_calls = NULL;
   cJSON *msg;

   cJSON_ArrayForEach(blk, content)
   {
      const char *type = jo_cstr(blk, "type");
      if (strcmp(type, "text") == 0)
      {
         const cJSON *t = cJSON_GetObjectItemCaseSensitive(blk, "text");
         if (cJSON_IsString(t))
            text = str_append_join(text, t->valuestring, text ? "\n" : NULL);
      }
      else if (strcmp(type, "tool_use") == 0)
      {
         const cJSON *input = cJSON_GetObjectItemCaseSensitive(blk, "input");
         char *args = input ? cJSON_PrintUnformatted(input) : NULL;
         cJSON *tc = cJSON_CreateObject();
         cJSON *fn;
         if (!tool_calls)
            tool_calls = cJSON_CreateArray();
         cJSON_AddStringToObject(tc, "id", jo_cstr(blk, "id"));
         cJSON_AddStringToObject(tc, "type", "function");
         fn = cJSON_AddObjectToObject(tc, "function");
         cJSON_AddStringToObject(fn, "name", jo_cstr(blk, "name"));
         cJSON_AddStringToObject(fn, "arguments", args ? args : "{}");
         free(args);
         cJSON_AddItemToArray(tool_calls, tc);
      }
   }

   msg = cJSON_CreateObject();
   cJSON_AddStringToObject(msg, "role", "assistant");
   if (text)
   {
      cJSON_AddStringToObject(msg, "content", text);
      free(text);
   }
   else
   {
      cJSON_AddNullToObject(msg, "content");
   }
   if (tool_calls)
      cJSON_AddItemToObject(msg, "tool_calls", tool_calls);
   cJSON_AddItemToArray(out, msg);
}

cJSON *anthropic_messages_to_openai(const cJSON *messages, const char *system_text)
{
   const cJSON *msg;
   cJSON *out;

   if (!cJSON_IsArray(messages))
      return NULL;

   out = cJSON_CreateArray();
   if (system_text && system_text[0])
   {
      cJSON *sm = cJSON_CreateObject();
      cJSON_AddStringToObject(sm, "role", "system");
      cJSON_AddStringToObject(sm, "content", system_text);
      cJSON_AddItemToArray(out, sm);
   }

   cJSON_ArrayForEach(msg, messages)
   {
      const char *role = jo_cstr(msg, "role");
      const cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");

      if (cJSON_IsString(content))
      {
         cJSON *m = cJSON_CreateObject();
         cJSON_AddStringToObject(m, "role", role[0] ? role : "user");
         cJSON_AddStringToObject(m, "content", content->valuestring);
         cJSON_AddItemToArray(out, m);
         continue;
      }
      if (!cJSON_IsArray(content))
         continue;

      if (strcmp(role, "assistant") == 0)
      {
         emit_assistant_message(content, out);
      }
      else
      {
         /* User turn: tool_result messages first (they must directly follow the
          * assistant tool_calls), then any text/image content. */
         emit_tool_result_messages(content, out);
         emit_user_content_message(content, out);
      }
   }
   return out;
}

cJSON *anthropic_tools_to_openai(const cJSON *anthropic_tools)
{
   const cJSON *tool;
   cJSON *out;

   if (!cJSON_IsArray(anthropic_tools) || cJSON_GetArraySize(anthropic_tools) == 0)
      return NULL;

   out = cJSON_CreateArray();
   cJSON_ArrayForEach(tool, anthropic_tools)
   {
      const char *name = jo_cstr(tool, "name");
      const char *desc;
      const cJSON *schema;
      cJSON *fn_wrap, *fn;

      if (!name[0])
         continue;
      fn_wrap = cJSON_CreateObject();
      cJSON_AddStringToObject(fn_wrap, "type", "function");
      fn = cJSON_AddObjectToObject(fn_wrap, "function");
      cJSON_AddStringToObject(fn, "name", name);
      desc = jo_cstr(tool, "description");
      if (desc[0])
         cJSON_AddStringToObject(fn, "description", desc);

      schema = cJSON_GetObjectItemCaseSensitive(tool, "input_schema");
      if (cJSON_IsObject(schema))
      {
         cJSON_AddItemToObject(fn, "parameters", cJSON_Duplicate(schema, 1));
      }
      else
      {
         cJSON *params = cJSON_AddObjectToObject(fn, "parameters");
         cJSON_AddStringToObject(params, "type", "object");
         cJSON_AddObjectToObject(params, "properties");
      }
      cJSON_AddItemToArray(out, fn_wrap);
   }

   if (cJSON_GetArraySize(out) == 0)
   {
      cJSON_Delete(out);
      return NULL;
   }
   return out;
}

cJSON *anthropic_tools_to_responses(const cJSON *anthropic_tools)
{
   cJSON *chat_tools = anthropic_tools_to_openai(anthropic_tools);
   cJSON *out, *tool;
   if (!chat_tools)
      return NULL;
   out = cJSON_CreateArray();
   cJSON_ArrayForEach(tool, chat_tools)
   {
      cJSON *fn = cJSON_GetObjectItemCaseSensitive(tool, "function");
      cJSON *flat;
      if (!cJSON_IsObject(fn))
         continue;
      flat = cJSON_CreateObject();
      cJSON_AddStringToObject(flat, "type", "function");
      cJSON *name = cJSON_GetObjectItemCaseSensitive(fn, "name");
      cJSON *desc = cJSON_GetObjectItemCaseSensitive(fn, "description");
      cJSON *params = cJSON_GetObjectItemCaseSensitive(fn, "parameters");
      if (cJSON_IsString(name))
         cJSON_AddStringToObject(flat, "name", name->valuestring);
      if (cJSON_IsString(desc))
         cJSON_AddStringToObject(flat, "description", desc->valuestring);
      if (params)
         cJSON_AddItemToObject(flat, "parameters", cJSON_Duplicate(params, 1));
      cJSON_AddItemToArray(out, flat);
   }
   cJSON_Delete(chat_tools);
   if (cJSON_GetArraySize(out) == 0)
   {
      cJSON_Delete(out);
      return NULL;
   }
   return out;
}

cJSON *anthropic_response_from_parsed(const char *resp_id, const char *model,
                                      const parsed_response_t *parsed)
{
   cJSON *obj = cJSON_CreateObject();
   cJSON *content, *usage;
   int n_calls = parsed ? parsed->call_count : 0;
   int i;

   cJSON_AddStringToObject(obj, "id", (resp_id && resp_id[0]) ? resp_id : "msg_aimee");
   cJSON_AddStringToObject(obj, "type", "message");
   cJSON_AddStringToObject(obj, "role", "assistant");
   cJSON_AddStringToObject(obj, "model", (model && model[0]) ? model : "aimee-primary");

   content = cJSON_AddArrayToObject(obj, "content");
   if (parsed && parsed->content && parsed->content[0])
   {
      cJSON *tb = cJSON_CreateObject();
      cJSON_AddStringToObject(tb, "type", "text");
      cJSON_AddStringToObject(tb, "text", parsed->content);
      cJSON_AddItemToArray(content, tb);
   }
   for (i = 0; i < n_calls; i++)
   {
      const parsed_tool_call_t *c = &parsed->calls[i];
      cJSON *ub = cJSON_CreateObject();
      cJSON *input = c->arguments ? cJSON_Parse(c->arguments) : NULL;
      if (!cJSON_IsObject(input))
      {
         if (input)
            cJSON_Delete(input);
         input = cJSON_CreateObject();
      }
      cJSON_AddStringToObject(ub, "type", "tool_use");
      cJSON_AddStringToObject(ub, "id", c->id[0] ? c->id : "toolu_aimee");
      cJSON_AddStringToObject(ub, "name", c->name);
      cJSON_AddItemToObject(ub, "input", input);
      cJSON_AddItemToArray(content, ub);
   }
   /* Anthropic responses always carry at least one content block. */
   if (cJSON_GetArraySize(content) == 0)
   {
      cJSON *tb = cJSON_CreateObject();
      cJSON_AddStringToObject(tb, "type", "text");
      cJSON_AddStringToObject(tb, "text", "");
      cJSON_AddItemToArray(content, tb);
   }

   cJSON_AddStringToObject(obj, "stop_reason", n_calls > 0 ? "tool_use" : "end_turn");
   cJSON_AddNullToObject(obj, "stop_sequence");

   usage = cJSON_AddObjectToObject(obj, "usage");
   cJSON_AddNumberToObject(usage, "input_tokens", parsed ? parsed->prompt_tokens : 0);
   cJSON_AddNumberToObject(usage, "output_tokens", parsed ? parsed->completion_tokens : 0);
   return obj;
}

/* --- Streaming translation ---------------------------------------------- */

struct anthropic_stream_xlate
{
   anthropic_sse_emit_fn emit;
   void *ctx;
   int block_open;        /* a content block is currently open */
   int block_is_tool;     /* the open block is tool_use (else text) */
   int cur_index;         /* Anthropic content index of the open block */
   int next_index;        /* index to assign to the next opened block */
   int cur_tc_index;      /* OpenAI tool_calls[].index bound to the open tool block */
   int any_tool;          /* at least one tool_use block was emitted */
   int input_tokens;      /* estimate at begin; overwritten by an upstream usage chunk */
   int output_tokens;     /* from a usage chunk, if any */
   int cache_read_tokens; /* OpenAI prompt_tokens_details.cached_tokens, if any */
   char stop_reason[24];  /* mapped finish_reason, "" until known */
};

/* Print `obj` and hand it to the emit callback as `event`'s data, then free it. */
static void xlate_emit(anthropic_stream_xlate_t *st, const char *event, cJSON *obj)
{
   char *json = cJSON_PrintUnformatted(obj);
   if (json)
   {
      st->emit(st->ctx, event, json);
      free(json);
   }
   cJSON_Delete(obj);
}

static void xlate_close_block(anthropic_stream_xlate_t *st)
{
   cJSON *o;
   if (!st->block_open)
      return;
   o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "type", "content_block_stop");
   cJSON_AddNumberToObject(o, "index", st->cur_index);
   xlate_emit(st, "content_block_stop", o);
   st->block_open = 0;
}

static void xlate_open_text_block(anthropic_stream_xlate_t *st)
{
   cJSON *o = cJSON_CreateObject();
   cJSON *cb;
   st->cur_index = st->next_index++;
   st->block_open = 1;
   st->block_is_tool = 0;
   st->cur_tc_index = -1;
   cJSON_AddStringToObject(o, "type", "content_block_start");
   cJSON_AddNumberToObject(o, "index", st->cur_index);
   cb = cJSON_AddObjectToObject(o, "content_block");
   cJSON_AddStringToObject(cb, "type", "text");
   cJSON_AddStringToObject(cb, "text", "");
   xlate_emit(st, "content_block_start", o);
}

static void xlate_open_tool_block(anthropic_stream_xlate_t *st, const char *id, const char *name,
                                  int tc_index)
{
   cJSON *o = cJSON_CreateObject();
   cJSON *cb;
   st->cur_index = st->next_index++;
   st->block_open = 1;
   st->block_is_tool = 1;
   st->cur_tc_index = tc_index;
   st->any_tool = 1;
   cJSON_AddStringToObject(o, "type", "content_block_start");
   cJSON_AddNumberToObject(o, "index", st->cur_index);
   cb = cJSON_AddObjectToObject(o, "content_block");
   cJSON_AddStringToObject(cb, "type", "tool_use");
   cJSON_AddStringToObject(cb, "id", (id && id[0]) ? id : "toolu_aimee");
   cJSON_AddStringToObject(cb, "name", name ? name : "");
   cJSON_AddObjectToObject(cb, "input");
   xlate_emit(st, "content_block_start", o);
}

static void xlate_emit_text_delta(anthropic_stream_xlate_t *st, const char *text)
{
   cJSON *o = cJSON_CreateObject();
   cJSON *d;
   cJSON_AddStringToObject(o, "type", "content_block_delta");
   cJSON_AddNumberToObject(o, "index", st->cur_index);
   d = cJSON_AddObjectToObject(o, "delta");
   cJSON_AddStringToObject(d, "type", "text_delta");
   cJSON_AddStringToObject(d, "text", text);
   xlate_emit(st, "content_block_delta", o);
}

static void xlate_emit_json_delta(anthropic_stream_xlate_t *st, const char *partial_json)
{
   cJSON *o = cJSON_CreateObject();
   cJSON *d;
   cJSON_AddStringToObject(o, "type", "content_block_delta");
   cJSON_AddNumberToObject(o, "index", st->cur_index);
   d = cJSON_AddObjectToObject(o, "delta");
   cJSON_AddStringToObject(d, "type", "input_json_delta");
   cJSON_AddStringToObject(d, "partial_json", partial_json);
   xlate_emit(st, "content_block_delta", o);
}

/* Map an OpenAI finish_reason onto an Anthropic stop_reason. */
static const char *map_stop_reason(const char *finish_reason)
{
   if (!finish_reason)
      return NULL;
   if (strcmp(finish_reason, "tool_calls") == 0 || strcmp(finish_reason, "function_call") == 0)
      return "tool_use";
   if (strcmp(finish_reason, "length") == 0)
      return "max_tokens";
   if (strcmp(finish_reason, "stop") == 0 || strcmp(finish_reason, "content_filter") == 0)
      return "end_turn";
   return "end_turn";
}

anthropic_stream_xlate_t *anthropic_stream_begin(const char *msg_id, const char *model,
                                                 int input_tokens, anthropic_sse_emit_fn emit,
                                                 void *ctx)
{
   anthropic_stream_xlate_t *st;
   cJSON *o, *m, *usage;

   if (!emit)
      return NULL;
   st = calloc(1, sizeof(*st));
   if (!st)
      return NULL;
   st->emit = emit;
   st->ctx = ctx;
   st->cur_tc_index = -1;
   st->input_tokens = input_tokens > 0 ? input_tokens : 0;

   o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "type", "message_start");
   m = cJSON_AddObjectToObject(o, "message");
   cJSON_AddStringToObject(m, "id", (msg_id && msg_id[0]) ? msg_id : "msg_aimee");
   cJSON_AddStringToObject(m, "type", "message");
   cJSON_AddStringToObject(m, "role", "assistant");
   cJSON_AddStringToObject(m, "model", (model && model[0]) ? model : "aimee-primary");
   cJSON_AddArrayToObject(m, "content");
   cJSON_AddNullToObject(m, "stop_reason");
   cJSON_AddNullToObject(m, "stop_sequence");
   usage = cJSON_AddObjectToObject(m, "usage");
   cJSON_AddNumberToObject(usage, "input_tokens", input_tokens > 0 ? input_tokens : 0);
   cJSON_AddNumberToObject(usage, "output_tokens", 0);
   xlate_emit(st, "message_start", o);
   return st;
}

/* Process the OpenAI delta.tool_calls array of one chunk. */
static void xlate_feed_tool_calls(anthropic_stream_xlate_t *st, const cJSON *tool_calls)
{
   const cJSON *tc;
   cJSON_ArrayForEach(tc, tool_calls)
   {
      const cJSON *idx = cJSON_GetObjectItemCaseSensitive(tc, "index");
      const cJSON *fn = cJSON_GetObjectItemCaseSensitive(tc, "function");
      int tc_index = cJSON_IsNumber(idx) ? idx->valueint : 0;
      const char *args = fn ? jo_cstr(fn, "arguments") : "";

      /* A new tool-call index starts a fresh Anthropic tool_use block. */
      if (!st->block_open || !st->block_is_tool || st->cur_tc_index != tc_index)
      {
         xlate_close_block(st);
         xlate_open_tool_block(st, jo_cstr(tc, "id"), fn ? jo_cstr(fn, "name") : "", tc_index);
      }
      /* Stream the argument fragment as partial JSON (may be empty on the
       * opening fragment that only carried id/name). */
      if (args && args[0])
         xlate_emit_json_delta(st, args);
   }
}

void anthropic_stream_feed_openai(anthropic_stream_xlate_t *st, const char *data_json)
{
   cJSON *root, *choices, *choice, *delta, *usage;
   const cJSON *content, *tool_calls, *finish;

   if (!st || !data_json || !data_json[0])
      return;
   if (strcmp(data_json, "[DONE]") == 0)
      return;
   root = cJSON_Parse(data_json);
   if (!root)
      return;

   usage = cJSON_GetObjectItemCaseSensitive(root, "usage");
   if (cJSON_IsObject(usage))
   {
      const cJSON *ct = cJSON_GetObjectItemCaseSensitive(usage, "completion_tokens");
      if (cJSON_IsNumber(ct))
         st->output_tokens = ct->valueint;
      /* Prefer the upstream-reported prompt count over the begin-time estimate. */
      const cJSON *pt = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens");
      if (cJSON_IsNumber(pt))
         st->input_tokens = pt->valueint;
      /* OpenAI prompt caching surfaces cached input under prompt_tokens_details. */
      const cJSON *ptd = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens_details");
      if (cJSON_IsObject(ptd))
      {
         const cJSON *cached = cJSON_GetObjectItemCaseSensitive(ptd, "cached_tokens");
         if (cJSON_IsNumber(cached))
            st->cache_read_tokens = cached->valueint;
      }
   }

   choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
   choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
   if (!choice)
   {
      cJSON_Delete(root);
      return;
   }

   delta = cJSON_GetObjectItemCaseSensitive(choice, "delta");
   content = delta ? cJSON_GetObjectItemCaseSensitive(delta, "content") : NULL;
   if (cJSON_IsString(content) && content->valuestring[0])
   {
      if (!st->block_open || st->block_is_tool)
      {
         xlate_close_block(st);
         xlate_open_text_block(st);
      }
      xlate_emit_text_delta(st, content->valuestring);
   }

   tool_calls = delta ? cJSON_GetObjectItemCaseSensitive(delta, "tool_calls") : NULL;
   if (cJSON_IsArray(tool_calls))
      xlate_feed_tool_calls(st, tool_calls);

   finish = cJSON_GetObjectItemCaseSensitive(choice, "finish_reason");
   if (cJSON_IsString(finish))
   {
      const char *sr = map_stop_reason(finish->valuestring);
      if (sr)
         snprintf(st->stop_reason, sizeof(st->stop_reason), "%s", sr);
   }

   cJSON_Delete(root);
}

void anthropic_stream_finish(anthropic_stream_xlate_t *st)
{
   cJSON *o, *d, *usage;
   const char *stop;

   if (!st)
      return;
   xlate_close_block(st);

   stop = st->stop_reason[0] ? st->stop_reason : (st->any_tool ? "tool_use" : "end_turn");
   o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "type", "message_delta");
   d = cJSON_AddObjectToObject(o, "delta");
   cJSON_AddStringToObject(d, "stop_reason", stop);
   cJSON_AddNullToObject(d, "stop_sequence");
   usage = cJSON_AddObjectToObject(o, "usage");
   cJSON_AddNumberToObject(usage, "output_tokens", st->output_tokens);
   xlate_emit(st, "message_delta", o);

   o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "type", "message_stop");
   xlate_emit(st, "message_stop", o);
}

void anthropic_stream_get_usage(const anthropic_stream_xlate_t *st, int *input_tokens,
                                int *output_tokens, int *cache_read_tokens)
{
   if (input_tokens)
      *input_tokens = st ? st->input_tokens : 0;
   if (output_tokens)
      *output_tokens = st ? st->output_tokens : 0;
   if (cache_read_tokens)
      *cache_read_tokens = st ? st->cache_read_tokens : 0;
}

void anthropic_stream_free(anthropic_stream_xlate_t *st)
{
   free(st);
}

/* --- Buffered response replay as Anthropic SSE (P2c streaming) -------------
 *
 * The streaming /v1/messages paths take this route when
 * `gateway_prevent_subagents` is ON: the upstream reply is buffered to
 * completion, the police function compacts `parsed.calls[]`, and this
 * helper replays the policed struct as a well-formed Anthropic SSE
 * sequence — so the client sees the same wire shape it would have seen
 * from the per-block translator, just buffered. Mirrors the wire shape
 * produced by `anthropic_response_from_parsed` (same call_count-derivation
 * rules, same usage fields). Pure (no I/O). */

static void replay_emit(anthropic_sse_emit_fn emit, void *ctx, const char *event, cJSON *obj)
{
   char *json = cJSON_PrintUnformatted(obj);
   if (json && emit)
   {
      emit(ctx, event, json);
   }
   free(json);
   cJSON_Delete(obj);
}

static cJSON *make_message_start(const char *msg_id, const char *model, const parsed_response_t *p)
{
   cJSON *o = cJSON_CreateObject();
   cJSON *msg;
   cJSON *usage;
   cJSON_AddStringToObject(o, "type", "message_start");
   msg = cJSON_AddObjectToObject(o, "message");
   cJSON_AddStringToObject(msg, "id", (msg_id && msg_id[0]) ? msg_id : "msg_aimee");
   cJSON_AddStringToObject(msg, "type", "message");
   cJSON_AddStringToObject(msg, "role", "assistant");
   cJSON_AddStringToObject(msg, "model", (model && model[0]) ? model : "aimee-primary");
   usage = cJSON_AddObjectToObject(msg, "usage");
   cJSON_AddNumberToObject(usage, "input_tokens", p ? p->prompt_tokens : 0);
   if (p && p->cache_write_tokens > 0)
      cJSON_AddNumberToObject(usage, "cache_creation_input_tokens", p->cache_write_tokens);
   if (p && p->cache_read_tokens > 0)
      cJSON_AddNumberToObject(usage, "cache_read_input_tokens", p->cache_read_tokens);
   return o;
}

static cJSON *make_content_block_start_text(int index)
{
   cJSON *o = cJSON_CreateObject();
   cJSON *cb;
   cJSON_AddStringToObject(o, "type", "content_block_start");
   cJSON_AddNumberToObject(o, "index", index);
   cb = cJSON_AddObjectToObject(o, "content_block");
   cJSON_AddStringToObject(cb, "type", "text");
   cJSON_AddStringToObject(cb, "text", "");
   return o;
}

static cJSON *make_content_block_delta_text(int index, const char *text)
{
   cJSON *o = cJSON_CreateObject();
   cJSON *d;
   cJSON_AddStringToObject(o, "type", "content_block_delta");
   cJSON_AddNumberToObject(o, "index", index);
   d = cJSON_AddObjectToObject(o, "delta");
   cJSON_AddStringToObject(d, "type", "text_delta");
   cJSON_AddStringToObject(d, "text", text ? text : "");
   return o;
}

static cJSON *make_content_block_stop(int index)
{
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "type", "content_block_stop");
   cJSON_AddNumberToObject(o, "index", index);
   return o;
}

static cJSON *make_content_block_start_tool(int index, const char *id, const char *name)
{
   cJSON *o = cJSON_CreateObject();
   cJSON *cb;
   cJSON_AddStringToObject(o, "type", "content_block_start");
   cJSON_AddNumberToObject(o, "index", index);
   cb = cJSON_AddObjectToObject(o, "content_block");
   cJSON_AddStringToObject(cb, "type", "tool_use");
   cJSON_AddStringToObject(cb, "id", id ? id : "toolu_aimee");
   cJSON_AddStringToObject(cb, "name", name ? name : "");
   cJSON_AddObjectToObject(cb, "input");
   return o;
}

static cJSON *make_content_block_delta_input_json(int index, const char *partial_json)
{
   cJSON *o = cJSON_CreateObject();
   cJSON *d;
   cJSON_AddStringToObject(o, "type", "content_block_delta");
   cJSON_AddNumberToObject(o, "index", index);
   d = cJSON_AddObjectToObject(o, "delta");
   cJSON_AddStringToObject(d, "type", "input_json_delta");
   cJSON_AddStringToObject(d, "partial_json", partial_json ? partial_json : "{}");
   return o;
}

static cJSON *make_message_delta(const parsed_response_t *p, int n_calls)
{
   cJSON *o = cJSON_CreateObject();
   cJSON *u;
   const char *stop =
       (p && p->stop_reason[0]) ? p->stop_reason : (n_calls > 0 ? "tool_use" : "end_turn");
   cJSON_AddStringToObject(o, "type", "message_delta");
   cJSON *delta = cJSON_AddObjectToObject(o, "delta");
   cJSON_AddStringToObject(delta, "stop_reason", stop);
   /* null (not "") when no stop sequence triggered — matches the Anthropic API and
    * the streaming path's message_delta (xlate_finish emits null here too). */
   cJSON_AddNullToObject(delta, "stop_sequence");
   u = cJSON_AddObjectToObject(o, "usage");
   cJSON_AddNumberToObject(u, "output_tokens", p ? p->completion_tokens : 0);
   if (p && p->cache_read_tokens > 0)
      cJSON_AddNumberToObject(u, "cache_read_input_tokens", p->cache_read_tokens);
   return o;
}

void emit_message_as_sse(const parsed_response_t *parsed, const char *msg_id, const char *model,
                         anthropic_sse_emit_fn emit, void *ctx)
{
   int n_calls;
   int index;

   if (!emit)
      return;
   n_calls = parsed ? parsed->call_count : 0;

   replay_emit(emit, ctx, "message_start", make_message_start(msg_id, model, parsed));

   /* Always emit one text content_block_start / (text_delta)? / stop pair —
    * even when parsed->content is empty, matching the buffered renderer's
    * empty-text-block fallback. */
   index = 0;
   replay_emit(emit, ctx, "content_block_start", make_content_block_start_text(index));
   if (parsed && parsed->content && parsed->content[0])
   {
      replay_emit(emit, ctx, "content_block_delta",
                  make_content_block_delta_text(index, parsed->content));
   }
   replay_emit(emit, ctx, "content_block_stop", make_content_block_stop(index));

   if (parsed)
   {
      int i;
      for (i = 0; i < n_calls; i++)
      {
         const parsed_tool_call_t *c = &parsed->calls[i];
         index++;
         replay_emit(emit, ctx, "content_block_start",
                     make_content_block_start_tool(index, c->id, c->name));
         replay_emit(emit, ctx, "content_block_delta",
                     make_content_block_delta_input_json(index, c->arguments));
         replay_emit(emit, ctx, "content_block_stop", make_content_block_stop(index));
      }
   }

   replay_emit(emit, ctx, "message_delta", make_message_delta(parsed, n_calls));
   replay_emit(emit, ctx, "message_stop", cJSON_CreateObject());
}
