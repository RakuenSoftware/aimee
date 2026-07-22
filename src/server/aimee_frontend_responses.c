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
#include "aimee_frontend.h"

#include "cJSON.h"

#include <stdio.h>
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

   const cJSON *mot = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "max_output_tokens");
   if (mot && cJSON_IsNumber(mot))
   {
      out->max_tokens = mot->valueint;
      out->has_max_tokens = 1;
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

   /* input items */
   const cJSON *input = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "input");
   if (input && cJSON_IsArray(input))
   {
      int tool_run = -1; /* index of a pending user msg accumulating tool_result blocks */
      const cJSON *item = NULL;
      cJSON_ArrayForEach(item, input)
      {
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
            const char *o = ostr(item, "output");
            b->tool_result = o ? cJSON_CreateString(o) : NULL;
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
            const char *args = ostr(item, "arguments");
            b->tool_input = args ? cJSON_Parse(args) : NULL;
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
            b->text = dupstr(ostr(item, "summary"));
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

   /* tools: flat function tools {type:function, name, description, parameters} */
   const cJSON *tools = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "tools");
   if (tools && cJSON_IsArray(tools))
   {
      const cJSON *t = NULL;
      cJSON_ArrayForEach(t, tools)
      {
         aimee_tool_t *tool = grow1((void **)&out->tools, &out->n_tools, sizeof(aimee_tool_t));
         if (!tool)
            goto oom;
         tool->name = dupstr(ostr(t, "name"));
         tool->description = dupstr(ostr(t, "description"));
         const cJSON *params = cJSON_GetObjectItemCaseSensitive((cJSON *)t, "parameters");
         tool->schema = params ? cJSON_Duplicate(params, 1) : NULL;
      }
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
   cJSON *out = cJSON_CreateObject();
   cJSON_AddStringToObject(out, "id", r->id ? r->id : "");
   cJSON_AddStringToObject(out, "object", "response");
   if (r->model)
      cJSON_AddStringToObject(out, "model", r->model);
   cJSON_AddStringToObject(out, "status",
                           r->stop_reason == AIMEE_STOP_MAX_TOKENS ? "incomplete" : "completed");

   cJSON *output = cJSON_AddArrayToObject(out, "output");
   for (int i = 0; i < r->n_content; i++)
   {
      const aimee_block_t *b = &r->content[i];
      if (b->type == AIMEE_BLK_TEXT)
      {
         cJSON *item = cJSON_CreateObject();
         cJSON_AddStringToObject(item, "type", "message");
         cJSON_AddStringToObject(item, "role", "assistant");
         cJSON *content = cJSON_AddArrayToObject(item, "content");
         cJSON *part = cJSON_CreateObject();
         cJSON_AddStringToObject(part, "type", "output_text");
         cJSON_AddStringToObject(part, "text", b->text ? b->text : "");
         cJSON_AddItemToArray(content, part);
         cJSON_AddItemToArray(output, item);
      }
      else if (b->type == AIMEE_BLK_TOOL_USE)
      {
         cJSON *item = cJSON_CreateObject();
         cJSON_AddStringToObject(item, "type", "function_call");
         cJSON_AddStringToObject(item, "call_id", b->tool_id ? b->tool_id : "");
         cJSON_AddStringToObject(item, "name", b->tool_name ? b->tool_name : "");
         char *args = b->tool_input ? cJSON_PrintUnformatted(b->tool_input) : NULL;
         cJSON_AddStringToObject(item, "arguments", args ? args : "{}");
         free(args);
         cJSON_AddItemToArray(output, item);
      }
      else if (b->type == AIMEE_BLK_THINKING)
      {
         cJSON *item = cJSON_CreateObject();
         cJSON_AddStringToObject(item, "type", "reasoning");
         cJSON_AddStringToObject(item, "summary", b->text ? b->text : "");
         cJSON_AddItemToArray(output, item);
      }
   }
   cJSON *usage = cJSON_AddObjectToObject(out, "usage");
   cJSON_AddNumberToObject(usage, "input_tokens", (double)r->usage_in);
   cJSON_AddNumberToObject(usage, "output_tokens", (double)r->usage_out);
   return out;
}
