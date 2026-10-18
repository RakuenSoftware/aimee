/* aimee_frontend_responses.c -- OpenAI Responses API (/v1/responses) -> IR. Lets a
 * Responses/codex CLIENT be served by any backend via the IR (no direct
 * translation). Completes the frontend matrix (Anthropic + OpenAI + Responses).
 * Parse only; render lands with the response path. See aimee_frontend.h.
 *
 * The Responses `input` is a FLAT item array: message / function_call /
 * function_call_output / reasoning. Per the grouping ruling (Option A), a
 * function_call becomes an assistant tool_use block and a function_call_output
 * merges into a user tool_result block, so the IR is identical to the equivalent
 * Anthropic/OpenAI turn. */
#include <aimee/translation/aimee_frontend.h>

#include <aimee/translation/aimee_backend.h> /* responses_reasoning_summary_text (shared) */

#include "cJSON.h"

#include <stdio.h>
#include <math.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static char *dupstr(const char *s)
{
   return s ? strdup(s) : NULL;
}

static const char *ostr(const cJSON *o, const char *k)
{
   const cJSON *it = cJSON_GetObjectItemCaseSensitive((cJSON *)o, k);
   return (it && cJSON_IsString(it)) ? it->valuestring : NULL;
}

static void *grow1(void **arr, int *n, size_t esz)
{
   void *p = realloc(*arr, (size_t)(*n + 1) * esz);
   if (!p)
      return NULL;
   *arr = p;
   void *slot = (char *)p + (size_t)(*n) * esz;
   memset(slot, 0, esz);
   (*n)++;
   return slot;
}

/* append a fresh message (by role) and return its index, or -1 on OOM */
static int new_message(aimee_request_t *out, const char *role)
{
   aimee_message_t *m = grow1((void **)&out->messages, &out->n_messages, sizeof(aimee_message_t));
   if (!m)
      return -1;
   m->role = dupstr(role);
   return out->n_messages - 1;
}

int responses_frontend_parse(const cJSON *req, aimee_request_t *out, char *err, size_t errn)
{
   if (out)
      memset(out, 0, sizeof *out);
   if (!req || !cJSON_IsObject(req) || !out)
   {
      if (err && errn)
         snprintf(err, errn, "responses_frontend_parse: null/non-object request");
      return -1;
   }
   out->frontend = AIMEE_WIRE_RESPONSES;
   out->raw = cJSON_Duplicate(req, 1);
   out->model = dupstr(ostr(req, "model"));

   const cJSON *mot = cJSON_GetObjectItemCaseSensitive(req, "max_output_tokens");
   if (mot && !cJSON_IsNull(mot))
   {
      long maximum = 0;
      if (aimee_ir_read_usage(req, "max_output_tokens", &maximum) || maximum < 1 ||
          maximum > INT_MAX)
      {
         if (err && errn)
            snprintf(err, errn, "invalid max_output_tokens");
         aimee_request_free(out);
         return -1;
      }
      out->max_tokens = (int)maximum;
      out->has_max_tokens = 1;
   }
   const cJSON *temperature = cJSON_GetObjectItemCaseSensitive(req, "temperature");
   if (temperature && !cJSON_IsNull(temperature))
   {
      if (!cJSON_IsNumber(temperature) || !isfinite(temperature->valuedouble) ||
          temperature->valuedouble < 0 || temperature->valuedouble > 2)
      {
         if (err && errn)
            snprintf(err, errn, "invalid temperature");
         aimee_request_free(out);
         return -1;
      }
      out->temperature = temperature->valuedouble;
      out->has_temperature = 1;
   }
   const cJSON *top_p = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "top_p");
   if (top_p && cJSON_IsNumber(top_p))
   {
      out->top_p = top_p->valuedouble;
      out->has_top_p = 1;
   }
   const cJSON *meta = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "metadata");
   if (meta && !cJSON_IsNull(meta))
      out->metadata = cJSON_Duplicate(meta, 1);
   const cJSON *stream = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "stream");
   out->stream = (stream && cJSON_IsTrue(stream)) ? 1 : 0;
   switch (openai_family_tool_choice_parse(
       cJSON_GetObjectItemCaseSensitive((cJSON *)req, "tool_choice"),
       cJSON_GetObjectItemCaseSensitive((cJSON *)req, "parallel_tool_calls"), 1, out, err, errn))
   {
   case 0:
      break;
   case -1:
      aimee_request_free(out);
      return -1;
   default:
      goto oom;
   }

   const cJSON *reasoning = cJSON_GetObjectItemCaseSensitive(req, "reasoning");
   const cJSON *effort = cJSON_GetObjectItemCaseSensitive(reasoning, "effort");
   const cJSON *summary = cJSON_GetObjectItemCaseSensitive(reasoning, "summary");
   if ((reasoning && !cJSON_IsNull(reasoning) && !cJSON_IsObject(reasoning)) ||
       (effort && !cJSON_IsNull(effort) &&
        (!cJSON_IsString(effort) ||
         aimee_ir_effort_parse(effort->valuestring, &out->reasoning_effort))) ||
       (summary && !cJSON_IsNull(summary) &&
        (!cJSON_IsString(summary) ||
         (strcmp(summary->valuestring, "auto") && strcmp(summary->valuestring, "concise") &&
          strcmp(summary->valuestring, "detailed")))))
   {
      aimee_request_free(out);
      return -1;
   }
   if (cJSON_IsString(summary))
   {
      out->reasoning_summary = dupstr(summary->valuestring);
      if (!out->reasoning_summary)
         goto oom;
   }

   /* instructions -> a single system text block */
   const char *instr = ostr(req, "instructions");
   if (instr && instr[0])
   {
      aimee_block_t *b = grow1((void **)&out->system, &out->n_system, sizeof(aimee_block_t));
      if (!b)
         goto oom;
      b->type = AIMEE_BLK_TEXT;
      b->text = dupstr(instr);
   }

   /* A Responses input can be the shorthand string form or the full flat item
    * array. Normalize the shorthand to the same user/text IR used by a message
    * item so protocol-neutral transforms (including first-message persona
    * placement) cannot depend on which equivalent client shape was chosen. */
   const cJSON *input = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "input");
   if (input && cJSON_IsString(input))
   {
      int mi = new_message(out, "user");
      if (mi < 0)
         goto oom;
      aimee_message_t *m = &out->messages[mi];
      aimee_block_t *b = grow1((void **)&m->blocks, &m->n_blocks, sizeof(aimee_block_t));
      if (!b)
         goto oom;
      b->type = AIMEE_BLK_TEXT;
      b->text = dupstr(input->valuestring);
      if (!b->text)
         goto oom;
   }
   else if (input && cJSON_IsArray(input))
   {
      int tool_run = -1; /* index of a pending user msg accumulating tool_result blocks */
      const cJSON *item = NULL;
      cJSON_ArrayForEach(item, input)
      {
         /* The published run input also accepts bare string items. Admit them
          * as distinct user messages, never as a flattened role transcript. */
         if (cJSON_IsString(item))
         {
            tool_run = -1;
            int mi = new_message(out, "user");
            if (mi < 0)
               goto oom;
            aimee_message_t *message = &out->messages[mi];
            aimee_block_t *block =
                grow1((void **)&message->blocks, &message->n_blocks, sizeof *block);
            if (!block)
               goto oom;
            block->type = AIMEE_BLK_TEXT;
            block->text = dupstr(item->valuestring);
            if (!block->text)
               goto oom;
            out->mutated = 1;
            continue;
         }
         const char *type = ostr(item, "type");
         if (type && strcmp(type, "function_call_output") == 0)
         {
            /* MERGE into a user message's tool_result blocks (grouping ruling) */
            if (tool_run < 0)
            {
               tool_run = new_message(out, "user");
               if (tool_run < 0)
                  goto oom;
            }
            aimee_message_t *tr = &out->messages[tool_run];
            aimee_block_t *b = grow1((void **)&tr->blocks, &tr->n_blocks, sizeof(aimee_block_t));
            if (!b)
               goto oom;
            b->type = AIMEE_BLK_TOOL_RESULT;
            b->raw = cJSON_Duplicate(item, 1);
            b->tool_id = dupstr(ostr(item, "call_id"));
            const cJSON *result = cJSON_GetObjectItemCaseSensitive(item, "output");
            b->tool_result = result ? cJSON_Duplicate(result, 1) : NULL;
            continue;
         }
         tool_run = -1; /* any non-output item ends the tool run */
         if (type && strcmp(type, "function_call") == 0)
         {
            int mi = new_message(out, "assistant");
            if (mi < 0)
               goto oom;
            aimee_message_t *m = &out->messages[mi];
            aimee_block_t *b = grow1((void **)&m->blocks, &m->n_blocks, sizeof(aimee_block_t));
            if (!b)
               goto oom;
            b->type = AIMEE_BLK_TOOL_USE;
            b->raw = cJSON_Duplicate(item, 1);
            b->tool_id = dupstr(ostr(item, "call_id"));
            b->tool_name = dupstr(ostr(item, "name"));
            /* Present when the client grouped this tool under a `namespace`; the
             * name is bare in that case and only the pair identifies the tool. */
            b->tool_namespace = dupstr(ostr(item, "namespace"));
            const char *args = ostr(item, "arguments");
            b->tool_input = args ? cJSON_ParseWithOpts(args, NULL, 1) : NULL;
            if (!cJSON_IsObject(b->tool_input))
            {
               if (err && errn)
                  snprintf(err, errn,
                           "completed function arguments require a complete JSON object");
               aimee_request_free(out);
               return -1;
            }
         }
         else if (type && strcmp(type, "reasoning") == 0)
         {
            int mi = new_message(out, "assistant");
            if (mi < 0)
               goto oom;
            aimee_message_t *m = &out->messages[mi];
            aimee_block_t *b = grow1((void **)&m->blocks, &m->n_blocks, sizeof(aimee_block_t));
            if (!b)
               goto oom;
            b->type = AIMEE_BLK_THINKING;
            b->raw = cJSON_Duplicate(item, 1);
            b->text = responses_reasoning_summary_text(item);
            b->thinking_id = dupstr(ostr(item, "id"));
            b->thinking_encrypted_content = dupstr(ostr(item, "encrypted_content"));
         }
         else /* "message" (default) */
         {
            int mi = new_message(out, ostr(item, "role"));
            if (mi < 0)
               goto oom;
            aimee_message_t *m = &out->messages[mi];
            m->raw = cJSON_Duplicate(item, 1);
            const cJSON *content = cJSON_GetObjectItemCaseSensitive((cJSON *)item, "content");
            const cJSON *part = NULL;
            if (content && cJSON_IsArray(content))
               cJSON_ArrayForEach(part, content)
               {
                  const char *pt = ostr(part, "type");
                  if (pt && (strcmp(pt, "input_text") == 0 || strcmp(pt, "output_text") == 0))
                  {
                     aimee_block_t *b =
                         grow1((void **)&m->blocks, &m->n_blocks, sizeof(aimee_block_t));
                     if (!b)
                        goto oom;
                     b->type = AIMEE_BLK_TEXT;
                     b->text = dupstr(ostr(part, "text"));
                  }
                  else if (pt && (!strcmp(pt, "input_image") || !strcmp(pt, "input_file")))
                  {
                     aimee_block_t *b =
                         grow1((void **)&m->blocks, &m->n_blocks, sizeof(aimee_block_t));
                     if (!b)
                        goto oom;
                     b->type = !strcmp(pt, "input_image") ? AIMEE_BLK_IMAGE : AIMEE_BLK_DOCUMENT;
                     const char *ref =
                         ostr(part, b->type == AIMEE_BLK_IMAGE ? "image_url" : "file_data");
                     if (!ref)
                        ref = ostr(part, "file_url");
                     if (!ref)
                        ref = ostr(part, "file_id");
                     b->media_ref = dupstr(ref);
                     if (!b->media_ref)
                        goto oom;
                  }
                  else
                  {
                     aimee_block_t *b =
                         grow1((void **)&m->blocks, &m->n_blocks, sizeof(aimee_block_t));
                     if (!b)
                        goto oom;
                     b->type = AIMEE_BLK_UNKNOWN;
                     b->raw = cJSON_Duplicate(part, 1);
                  }
               }
            else if (cJSON_IsString(content))
            {
               aimee_block_t *b = grow1((void **)&m->blocks, &m->n_blocks, sizeof(aimee_block_t));
               if (!b)
                  goto oom;
               b->type = AIMEE_BLK_TEXT;
               b->text = dupstr(content->valuestring);
            }
         }
      }
   }

   /* tools: flat function tools {type:function, name, description, parameters}
    *
    * ONLY named function tools. A Codex client also sends provider-native tool
    * TYPES -- custom, local_shell, web_search, image_generation -- which carry no
    * top-level `name`. Admitting them produced an IR tool with a NULL name, which
    * reached the provider as `tools[N].name: ""` and made it reject the WHOLE
    * request: 400 empty_string, every tool in the catalog lost, the turn dead. With
    * a real Codex client that was tools[16] of 17, so the gateway path failed on
    * every request that carried a catalog.
    *
    * openai_parse_responses_to_chat -- the legacy translator this path runs ahead of
    * -- has always filtered exactly this, and says so ("Codex's
    * namespace/web_search/image_generation tool *types* are dropped"). The two
    * disagreeing is what made the failure depend on which translator handled the
    * request. */
   const cJSON *tools = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "tools");
   if (tools && cJSON_IsArray(tools))
   {
      const cJSON *t = NULL;
      cJSON_ArrayForEach(t, tools)
      {
         aimee_tool_t *tool = grow1((void **)&out->tools, &out->n_tools, sizeof *tool);
         if (!tool || openai_ir_parse_tool(t, tool, 0))
         {
            if (err && errn)
               snprintf(err, errn, "invalid or unsupported typed tool catalog");
            aimee_request_free(out);
            return -1;
         }
      }
   }
   if (openai_family_output_format_parse(
           cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(req, "text"),
                                            "format"),
           1, out, err, errn))
   {
      aimee_request_free(out);
      return -1;
   }
   return 0;

oom:
   if (err && errn)
      snprintf(err, errn, "responses_frontend_parse: out of memory");
   aimee_request_free(out);
   return -1;
}

cJSON *responses_frontend_render(const aimee_response_t *r)
{
   if (!r)
      return NULL;
   if (r->error_message)
   {
      cJSON *failure = cJSON_CreateObject();
      cJSON *error = cJSON_AddObjectToObject(failure, "error");
      if (!failure || !error || !cJSON_AddStringToObject(error, "type", "upstream_error") ||
          !cJSON_AddStringToObject(error, "message", r->error_message))
      {
         cJSON_Delete(failure);
         return NULL;
      }
      return failure;
   }
   cJSON *out = cJSON_CreateObject();
   cJSON_AddStringToObject(out, "id", r->id ? r->id : "");
   cJSON_AddStringToObject(out, "object", "response");
   if (r->model)
      cJSON_AddStringToObject(out, "model", r->model);
   cJSON_AddStringToObject(out, "status",
                           r->stop_reason == AIMEE_STOP_MAX_TOKENS ? "incomplete" : "completed");

   if (r->stop_reason == AIMEE_STOP_MAX_TOKENS)
      cJSON_AddStringToObject(cJSON_AddObjectToObject(out, "incomplete_details"), "reason",
                              "max_output_tokens");
   cJSON *output = cJSON_AddArrayToObject(out, "output");
   cJSON *last_message = NULL;
   int last_group = -1;
   for (int i = 0; i < r->n_content; i++)
   {
      const aimee_block_t *b = &r->content[i];
      if (b->type == AIMEE_BLK_TEXT)
      {
         cJSON *item = last_message;
         if (!item || last_group != b->message_group)
         {
            item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "type", "message");
            cJSON_AddStringToObject(item, "role", "assistant");
            cJSON_AddArrayToObject(item, "content");
            cJSON_AddItemToArray(output, item);
         }
         cJSON *part = cJSON_CreateObject();
         cJSON_AddStringToObject(part, "type", "output_text");
         cJSON_AddStringToObject(part, "text", b->text ? b->text : "");
         cJSON_AddArrayToObject(part, "annotations");
         cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(item, "content"), part);
         last_message = item;
         last_group = b->message_group;
      }
      else if (b->type == AIMEE_BLK_TOOL_USE)
      {
         last_message = NULL;
         cJSON *item = cJSON_CreateObject();
         cJSON_AddStringToObject(item, "type", "function_call");
         cJSON_AddStringToObject(item, "call_id", b->tool_id ? b->tool_id : "");
         cJSON_AddStringToObject(item, "name", b->tool_name ? b->tool_name : "");
         if (b->tool_namespace && b->tool_namespace[0])
            cJSON_AddStringToObject(item, "namespace", b->tool_namespace);
         char *args = b->tool_input ? cJSON_PrintUnformatted(b->tool_input) : NULL;
         cJSON_AddStringToObject(item, "arguments", args ? args : "{}");
         free(args);
         cJSON_AddItemToArray(output, item);
      }
      else if (b->type == AIMEE_BLK_THINKING)
      {
         last_message = NULL;
         cJSON *item = cJSON_CreateObject();
         cJSON_AddStringToObject(item, "type", "reasoning");
         if (b->thinking_id)
            cJSON_AddStringToObject(item, "id", b->thinking_id);
         if (b->thinking_encrypted_content)
            cJSON_AddStringToObject(item, "encrypted_content", b->thinking_encrypted_content);
         cJSON *summary = cJSON_AddArrayToObject(item, "summary");
         if (b->text && b->text[0])
         {
            cJSON *part = cJSON_CreateObject();
            cJSON_AddStringToObject(part, "type", "summary_text");
            cJSON_AddStringToObject(part, "text", b->text);
            cJSON_AddItemToArray(summary, part);
         }
         cJSON_AddItemToArray(output, item);
      }
      else
      {
         /* This client wire cannot represent these admitted native blocks. */
         cJSON_Delete(out);
         return NULL;
      }
   }
   cJSON *usage = cJSON_AddObjectToObject(out, "usage");
   cJSON_AddNumberToObject(usage, "input_tokens", (double)r->usage_in);
   cJSON_AddNumberToObject(usage, "output_tokens", (double)r->usage_out);
   cJSON *cached = cJSON_AddObjectToObject(usage, "input_tokens_details");
   cJSON_AddNumberToObject(cached, "cached_tokens", (double)r->usage_cache_read);
   cJSON *reasoning = cJSON_AddObjectToObject(usage, "output_tokens_details");
   cJSON_AddNumberToObject(reasoning, "reasoning_tokens", (double)r->usage_reasoning);
   return out;
}
