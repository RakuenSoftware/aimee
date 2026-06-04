/* openai_shape.c: OpenAI-compatible JSON shaping helpers (no sockets, no network). */
#include "openai_shape.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Parse helpers ─────────────────────────────────────────────────────── */

static int parse_model(const cJSON *root, char *model, size_t model_n)
{
   const cJSON *m = cJSON_GetObjectItemCaseSensitive(root, "model");
   if (cJSON_IsString(m) && m->valuestring && m->valuestring[0])
      snprintf(model, model_n, "%s", m->valuestring);
   else
      snprintf(model, model_n, "%s", "aimee");
   return 0;
}

static int parse_stream(const cJSON *root)
{
   const cJSON *s = cJSON_GetObjectItemCaseSensitive(root, "stream");
   return cJSON_IsTrue(s) ? 1 : 0;
}

/* ── openai_parse_chat_request ─────────────────────────────────────────── */

int openai_parse_chat_request(const char *body, char *model, size_t model_n, char **prompt_out,
                              int *stream_out)
{
   if (!body || !prompt_out)
      return -1;
   *prompt_out = NULL;
   if (stream_out)
      *stream_out = 0;

   cJSON *root = cJSON_Parse(body);
   if (!root)
      return -1;

   if (model && model_n)
      parse_model(root, model, model_n);
   if (stream_out)
      *stream_out = parse_stream(root);

   const cJSON *messages = cJSON_GetObjectItemCaseSensitive(root, "messages");
   if (!cJSON_IsArray(messages))
   {
      cJSON_Delete(root);
      return -1;
   }

   /* Dynamically grow a heap buffer for the flattened transcript. */
   size_t buf_cap = 256;
   size_t buf_len = 0;
   char *buf = malloc(buf_cap);
   if (!buf)
   {
      cJSON_Delete(root);
      return -1;
   }
   buf[0] = '\0';

   int found = 0;
   cJSON *msg;
   cJSON_ArrayForEach(msg, messages)
   {
      const cJSON *roleItem = cJSON_GetObjectItemCaseSensitive(msg, "role");
      const char *role = cJSON_IsString(roleItem) ? roleItem->valuestring : "user";

      const cJSON *contentItem = cJSON_GetObjectItemCaseSensitive(msg, "content");
      if (!cJSON_IsString(contentItem) || !contentItem->valuestring || !contentItem->valuestring[0])
         continue;

      const char *content = contentItem->valuestring;
      size_t need = strlen(role) + strlen(content) + 4; /* ": \n\0" */
      while (buf_len + need > buf_cap)
      {
         buf_cap *= 2;
         char *tmp = realloc(buf, buf_cap);
         if (!tmp)
         {
            free(buf);
            cJSON_Delete(root);
            return -1;
         }
         buf = tmp;
      }
      buf_len += snprintf(buf + buf_len, buf_cap - buf_len, "%s: %s\n", role, content);
      found = 1;
   }

   cJSON_Delete(root);

   if (!found)
   {
      free(buf);
      *prompt_out = NULL;
      return -1;
   }

   *prompt_out = buf;
   return 0;
}

/* ── openai_parse_completion_request ─────────────────────────────────── */

int openai_parse_completion_request(const char *body, char *model, size_t model_n,
                                    char **prompt_out, int *stream_out)
{
   if (!body || !prompt_out)
      return -1;
   *prompt_out = NULL;
   if (stream_out)
      *stream_out = 0;

   cJSON *root = cJSON_Parse(body);
   if (!root)
      return -1;

   if (model && model_n)
      parse_model(root, model, model_n);
   if (stream_out)
      *stream_out = parse_stream(root);

   const cJSON *promptItem = cJSON_GetObjectItemCaseSensitive(root, "prompt");
   if (!cJSON_IsString(promptItem) || !promptItem->valuestring || !promptItem->valuestring[0])
   {
      cJSON_Delete(root);
      return -1;
   }

   char *dup = strdup(promptItem->valuestring);
   cJSON_Delete(root);
   if (!dup)
      return -1;

   *prompt_out = dup;
   return 0;
}

/* ── openai_parse_responses_request ──────────────────────────────────────── */

/* Append "role: text\n" to a growing heap buffer. Returns 0 on success, -1 on
 * OOM (buffer left freed and *buf NULL). Skips empty text. */
static int responses_append(char **buf, size_t *cap, size_t *len, const char *role,
                            const char *text)
{
   if (!text || !text[0])
      return 0;
   size_t need = strlen(role) + strlen(text) + 4; /* ": \n\0" */
   while (*len + need > *cap)
   {
      size_t ncap = *cap * 2;
      char *tmp = realloc(*buf, ncap);
      if (!tmp)
      {
         free(*buf);
         *buf = NULL;
         return -1;
      }
      *buf = tmp;
      *cap = ncap;
   }
   *len += (size_t)snprintf(*buf + *len, *cap - *len, "%s: %s\n", role, text);
   return 0;
}

/* Flatten one Responses `content` value (a string, or an array of parts like
 * {"type":"input_text","text":"…"}) for the given role into buf. */
static int responses_append_content(char **buf, size_t *cap, size_t *len, const char *role,
                                    const cJSON *content)
{
   if (cJSON_IsString(content))
      return responses_append(buf, cap, len, role, content->valuestring);
   if (cJSON_IsArray(content))
   {
      const cJSON *part;
      cJSON_ArrayForEach(part, content)
      {
         const cJSON *t = cJSON_GetObjectItemCaseSensitive(part, "text");
         if (cJSON_IsString(t) && responses_append(buf, cap, len, role, t->valuestring) != 0)
            return -1;
      }
   }
   return 0;
}

int openai_parse_responses_request(const char *body, char *model, size_t model_n, char **prompt_out,
                                   char *prev_id, size_t prev_id_n, int *stream_out)
{
   if (!body || !prompt_out)
      return -1;
   *prompt_out = NULL;
   if (prev_id && prev_id_n)
      prev_id[0] = '\0';
   if (stream_out)
      *stream_out = 0;

   cJSON *root = cJSON_Parse(body);
   if (!root)
      return -1;

   if (model && model_n)
      parse_model(root, model, model_n);
   if (stream_out)
      *stream_out = parse_stream(root);
   if (prev_id && prev_id_n)
   {
      const cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "previous_response_id");
      if (cJSON_IsString(p) && p->valuestring)
         snprintf(prev_id, prev_id_n, "%s", p->valuestring);
   }

   const cJSON *input = cJSON_GetObjectItemCaseSensitive(root, "input");
   size_t buf_cap = 256, buf_len = 0;
   char *buf = malloc(buf_cap);
   if (!buf)
   {
      cJSON_Delete(root);
      return -1;
   }
   buf[0] = '\0';

   int ok = 1;
   if (cJSON_IsString(input))
   {
      ok = responses_append(&buf, &buf_cap, &buf_len, "user", input->valuestring) == 0;
   }
   else if (cJSON_IsArray(input))
   {
      const cJSON *item;
      cJSON_ArrayForEach(item, input)
      {
         if (cJSON_IsString(item))
         {
            if (responses_append(&buf, &buf_cap, &buf_len, "user", item->valuestring) != 0)
            {
               ok = 0;
               break;
            }
            continue;
         }
         const cJSON *roleItem = cJSON_GetObjectItemCaseSensitive(item, "role");
         const char *role = cJSON_IsString(roleItem) ? roleItem->valuestring : "user";
         const cJSON *content = cJSON_GetObjectItemCaseSensitive(item, "content");
         if (responses_append_content(&buf, &buf_cap, &buf_len, role, content) != 0)
         {
            ok = 0;
            break;
         }
      }
   }
   cJSON_Delete(root);

   if (!ok || !buf || buf_len == 0)
   {
      free(buf);
      *prompt_out = NULL;
      return -1;
   }
   *prompt_out = buf;
   return 0;
}

/* ── Codex Responses request → OpenAI chat (full-parity ingress) ──────────────
 *
 * Codex drives aimee like any other model: it POSTs a Responses-API request with
 * `instructions`, a structured `input` history (message / function_call /
 * function_call_output items), and OpenAI `function` tools. This converts that
 * into the OpenAI chat-completions shape so the request can run on the primary
 * agent's provider and the model's tool calls can be relayed back to Codex. */

/* Join all text parts of a Responses `content` array into one heap string.
 * Handles both {type:input_text|output_text|text, text:…} parts and a bare
 * string content. Returns "" (heap) when empty. NULL on OOM. */
static char *responses_join_content_text(const cJSON *content)
{
   size_t cap = 64, len = 0;
   char *out = malloc(cap);
   if (!out)
      return NULL;
   out[0] = '\0';
   if (cJSON_IsString(content))
   {
      free(out);
      return strdup(content->valuestring ? content->valuestring : "");
   }
   if (cJSON_IsArray(content))
   {
      const cJSON *part;
      cJSON_ArrayForEach(part, content)
      {
         const char *t = NULL;
         if (cJSON_IsString(part))
            t = part->valuestring;
         else
         {
            const cJSON *tx = cJSON_GetObjectItemCaseSensitive(part, "text");
            if (cJSON_IsString(tx))
               t = tx->valuestring;
         }
         if (!t || !t[0])
            continue;
         size_t need = strlen(t) + 1;
         while (len + need > cap)
         {
            size_t ncap = cap * 2;
            char *tmp = realloc(out, ncap);
            if (!tmp)
            {
               free(out);
               return NULL;
            }
            out = tmp;
            cap = ncap;
         }
         len += (size_t)snprintf(out + len, cap - len, "%s", t);
      }
   }
   return out;
}

/* Stringify a function_call_output `output` field (Codex sends a string, but be
 * tolerant of an object/array). Returns heap string; NULL on OOM. */
static char *responses_output_to_string(const cJSON *output)
{
   if (!output)
      return strdup("");
   if (cJSON_IsString(output))
      return strdup(output->valuestring ? output->valuestring : "");
   char *s = cJSON_PrintUnformatted(output);
   return s ? s : strdup("");
}

int openai_parse_responses_to_chat(const char *body, char *model, size_t model_n,
                                   char **instructions_out, struct cJSON **messages_out,
                                   struct cJSON **tools_out, int *stream_out)
{
   if (messages_out)
      *messages_out = NULL;
   if (tools_out)
      *tools_out = NULL;
   if (instructions_out)
      *instructions_out = NULL;
   if (stream_out)
      *stream_out = 0;
   if (!body || !messages_out)
      return -1;

   cJSON *root = cJSON_Parse(body);
   if (!root)
      return -1;

   if (model && model_n)
      parse_model(root, model, model_n);
   if (stream_out)
      *stream_out = parse_stream(root);

   if (instructions_out)
   {
      const cJSON *ins = cJSON_GetObjectItemCaseSensitive(root, "instructions");
      if (cJSON_IsString(ins) && ins->valuestring && ins->valuestring[0])
         *instructions_out = strdup(ins->valuestring);
   }

   cJSON *messages = cJSON_CreateArray();
   if (!messages)
   {
      cJSON_Delete(root);
      free(instructions_out ? *instructions_out : NULL);
      if (instructions_out)
         *instructions_out = NULL;
      return -1;
   }

   const cJSON *input = cJSON_GetObjectItemCaseSensitive(root, "input");
   const cJSON *item;
   cJSON_ArrayForEach(item, input)
   {
      if (cJSON_IsString(item))
      {
         cJSON *m = cJSON_CreateObject();
         cJSON_AddStringToObject(m, "role", "user");
         cJSON_AddStringToObject(m, "content", item->valuestring ? item->valuestring : "");
         cJSON_AddItemToArray(messages, m);
         continue;
      }
      const cJSON *typ = cJSON_GetObjectItemCaseSensitive(item, "type");
      const char *type = cJSON_IsString(typ) ? typ->valuestring : "message";

      if (strcmp(type, "function_call") == 0)
      {
         const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
         const cJSON *args = cJSON_GetObjectItemCaseSensitive(item, "arguments");
         const cJSON *cid = cJSON_GetObjectItemCaseSensitive(item, "call_id");
         cJSON *m = cJSON_CreateObject();
         cJSON_AddStringToObject(m, "role", "assistant");
         cJSON_AddNullToObject(m, "content");
         cJSON *tcs = cJSON_AddArrayToObject(m, "tool_calls");
         cJSON *tc = cJSON_CreateObject();
         cJSON_AddStringToObject(tc, "id", cJSON_IsString(cid) ? cid->valuestring : "");
         cJSON_AddStringToObject(tc, "type", "function");
         cJSON *fn = cJSON_AddObjectToObject(tc, "function");
         cJSON_AddStringToObject(fn, "name", cJSON_IsString(name) ? name->valuestring : "");
         cJSON_AddStringToObject(fn, "arguments", cJSON_IsString(args) ? args->valuestring : "{}");
         cJSON_AddItemToArray(tcs, tc);
         cJSON_AddItemToArray(messages, m);
         continue;
      }
      if (strcmp(type, "function_call_output") == 0)
      {
         const cJSON *cid = cJSON_GetObjectItemCaseSensitive(item, "call_id");
         const cJSON *out = cJSON_GetObjectItemCaseSensitive(item, "output");
         char *text = responses_output_to_string(out);
         cJSON *m = cJSON_CreateObject();
         cJSON_AddStringToObject(m, "role", "tool");
         cJSON_AddStringToObject(m, "tool_call_id", cJSON_IsString(cid) ? cid->valuestring : "");
         cJSON_AddStringToObject(m, "content", text ? text : "");
         free(text);
         cJSON_AddItemToArray(messages, m);
         continue;
      }
      if (strcmp(type, "message") == 0)
      {
         const cJSON *roleItem = cJSON_GetObjectItemCaseSensitive(item, "role");
         const char *role = cJSON_IsString(roleItem) ? roleItem->valuestring : "user";
         /* OpenAI chat has no "developer" role; fold it into system. */
         if (strcmp(role, "developer") == 0)
            role = "system";
         const cJSON *content = cJSON_GetObjectItemCaseSensitive(item, "content");
         char *text = responses_join_content_text(content);
         cJSON *m = cJSON_CreateObject();
         cJSON_AddStringToObject(m, "role", role);
         cJSON_AddStringToObject(m, "content", text ? text : "");
         free(text);
         cJSON_AddItemToArray(messages, m);
         continue;
      }
      /* reasoning / item_reference / unknown item types: skip. */
   }

   /* Tools: keep only OpenAI `function` tools, reshaped into chat form
    * {type:function, function:{name,description,parameters}}. Codex's
    * namespace/web_search/image_generation tool *types* are dropped. */
   cJSON *tools = NULL;
   const cJSON *in_tools = cJSON_GetObjectItemCaseSensitive(root, "tools");
   if (cJSON_IsArray(in_tools))
   {
      const cJSON *t;
      cJSON_ArrayForEach(t, in_tools)
      {
         const cJSON *tt = cJSON_GetObjectItemCaseSensitive(t, "type");
         if (!cJSON_IsString(tt) || strcmp(tt->valuestring, "function") != 0)
            continue;
         const cJSON *name = cJSON_GetObjectItemCaseSensitive(t, "name");
         if (!cJSON_IsString(name))
            continue;
         if (!tools)
            tools = cJSON_CreateArray();
         cJSON *ct = cJSON_CreateObject();
         cJSON_AddStringToObject(ct, "type", "function");
         cJSON *fn = cJSON_AddObjectToObject(ct, "function");
         cJSON_AddStringToObject(fn, "name", name->valuestring);
         const cJSON *desc = cJSON_GetObjectItemCaseSensitive(t, "description");
         if (cJSON_IsString(desc))
            cJSON_AddStringToObject(fn, "description", desc->valuestring);
         const cJSON *params = cJSON_GetObjectItemCaseSensitive(t, "parameters");
         if (params)
            cJSON_AddItemToObject(fn, "parameters", cJSON_Duplicate(params, 1));
         cJSON_AddItemToArray(tools, ct);
      }
   }

   cJSON_Delete(root);
   *messages_out = messages;
   if (tools_out)
      *tools_out = tools;
   else
      cJSON_Delete(tools);
   return 0;
}

/* ── optional sampling-field readers ─────────────────────────────────────── */

double openai_request_double(const char *body, const char *field, double dflt, double hi)
{
   if (!body || !field)
      return dflt;
   cJSON *root = cJSON_Parse(body);
   if (!root)
      return dflt;
   double out = dflt;
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, field);
   if (cJSON_IsNumber(v))
   {
      double d = v->valuedouble;
      if (d >= 0.0 && d <= hi)
         out = d;
   }
   cJSON_Delete(root);
   return out;
}

int openai_request_int(const char *body, const char *field, int dflt, int hi)
{
   if (!body || !field)
      return dflt;
   cJSON *root = cJSON_Parse(body);
   if (!root)
      return dflt;
   int out = dflt;
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, field);
   if (cJSON_IsNumber(v))
   {
      double d = v->valuedouble;
      if (d >= 1.0 && d <= (double)hi)
         out = (int)d;
   }
   cJSON_Delete(root);
   return out;
}

int openai_request_bool(const char *body, const char *field)
{
   if (!body || !field)
      return 0;
   cJSON *root = cJSON_Parse(body);
   if (!root)
      return 0;
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, field);
   int out = cJSON_IsTrue(v) ? 1 : 0;
   cJSON_Delete(root);
   return out;
}

/* ── embeddings ──────────────────────────────────────────────────────────── */

void openai_free_inputs(char **inputs, int n)
{
   if (!inputs)
      return;
   for (int i = 0; i < n; i++)
      free(inputs[i]);
   free(inputs);
}

int openai_parse_embeddings_request(const char *body, char *model, size_t model_n,
                                    char ***inputs_out, int *n_out)
{
   if (!inputs_out || !n_out)
      return -1;
   *inputs_out = NULL;
   *n_out = 0;
   if (!body)
      return -1;

   cJSON *root = cJSON_Parse(body);
   if (!root)
      return -1;
   if (model && model_n)
      parse_model(root, model, model_n);

   const cJSON *input = cJSON_GetObjectItemCaseSensitive(root, "input");

   /* Count how many non-empty string inputs we have. */
   int count = 0;
   if (cJSON_IsString(input) && input->valuestring && input->valuestring[0])
      count = 1;
   else if (cJSON_IsArray(input))
   {
      const cJSON *el;
      cJSON_ArrayForEach(el, input) if (cJSON_IsString(el) && el->valuestring && el->valuestring[0])
          count++;
   }
   if (count == 0)
   {
      cJSON_Delete(root);
      return -1;
   }

   char **arr = calloc((size_t)count, sizeof(char *));
   if (!arr)
   {
      cJSON_Delete(root);
      return -1;
   }
   int k = 0;
   if (cJSON_IsString(input))
   {
      arr[k++] = strdup(input->valuestring);
   }
   else
   {
      const cJSON *el;
      cJSON_ArrayForEach(el, input)
      {
         if (k >= count)
            break;
         if (cJSON_IsString(el) && el->valuestring && el->valuestring[0])
            arr[k++] = strdup(el->valuestring);
      }
   }
   cJSON_Delete(root);

   /* If any strdup failed, unwind. */
   for (int i = 0; i < k; i++)
   {
      if (!arr[i])
      {
         openai_free_inputs(arr, k);
         return -1;
      }
   }

   *inputs_out = arr;
   *n_out = k;
   return 0;
}

int openai_format_embeddings(const char *model, const float *const *vecs, const int *dims, int n,
                             int prompt_tokens, char *resp, int cap)
{
   if (!resp || cap <= 0 || n < 0 || (n > 0 && (!vecs || !dims)))
      return -1;

   cJSON *root = cJSON_CreateObject();
   if (!root)
      return -1;
   cJSON_AddStringToObject(root, "object", "list");

   cJSON *data = cJSON_CreateArray();
   if (!data)
   {
      cJSON_Delete(root);
      return -1;
   }
   cJSON_AddItemToObject(root, "data", data);

   for (int i = 0; i < n; i++)
   {
      cJSON *item = cJSON_CreateObject();
      if (!item)
      {
         cJSON_Delete(root);
         return -1;
      }
      cJSON_AddStringToObject(item, "object", "embedding");
      cJSON_AddNumberToObject(item, "index", (double)i);
      cJSON *emb = cJSON_CreateFloatArray(vecs[i], dims[i] > 0 ? dims[i] : 0);
      if (!emb)
      {
         cJSON_Delete(item);
         cJSON_Delete(root);
         return -1;
      }
      cJSON_AddItemToObject(item, "embedding", emb);
      cJSON_AddItemToArray(data, item);
   }

   cJSON_AddStringToObject(root, "model", model ? model : "");
   cJSON *usage = cJSON_CreateObject();
   if (!usage)
   {
      cJSON_Delete(root);
      return -1;
   }
   cJSON_AddItemToObject(root, "usage", usage);
   cJSON_AddNumberToObject(usage, "prompt_tokens", (double)prompt_tokens);
   cJSON_AddNumberToObject(usage, "total_tokens", (double)prompt_tokens);

   char *s = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!s)
      return -1;
   int len = snprintf(resp, (size_t)cap, "%s", s);
   free(s);
   return (len < 0 || len >= cap) ? -1 : len;
}

/* ── openai_format_models_list ────────────────────────────────────────── */

int openai_format_models_list(const char *const *ids, int n, const char *owner, char *resp, int cap)
{
   if (!resp || cap <= 0 || !ids || n <= 0)
      return -1;

   cJSON *root = cJSON_CreateObject();
   if (!root)
      return -1;

   cJSON_AddStringToObject(root, "object", "list");

   cJSON *data = cJSON_CreateArray();
   if (!data)
   {
      cJSON_Delete(root);
      return -1;
   }
   cJSON_AddItemToObject(root, "data", data);

   for (int i = 0; i < n; i++)
   {
      cJSON *item = cJSON_CreateObject();
      if (!item)
      {
         cJSON_Delete(root);
         return -1;
      }
      cJSON_AddStringToObject(item, "id", ids[i] ? ids[i] : "");
      cJSON_AddStringToObject(item, "object", "model");
      cJSON_AddStringToObject(item, "owned_by", owner ? owner : "");
      cJSON_AddItemToArray(data, item);
   }

   char *s = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!s)
      return -1;

   int len = snprintf(resp, (size_t)cap, "%s", s);
   free(s);
   return (len < 0 || len >= cap) ? -1 : len;
}

/* ── openai_format_chat_completion ────────────────────────────────────── */

int openai_format_chat_completion(const char *id, const char *model, const char *content,
                                  long created, int prompt_tokens, int completion_tokens,
                                  char *resp, int cap)
{
   if (!resp || cap <= 0)
      return -1;

   cJSON *root = cJSON_CreateObject();
   if (!root)
      return -1;

   cJSON_AddStringToObject(root, "id", id ? id : "");
   cJSON_AddStringToObject(root, "object", "chat.completion");
   cJSON_AddNumberToObject(root, "created", (double)created);
   cJSON_AddStringToObject(root, "model", model ? model : "");

   cJSON *choices = cJSON_CreateArray();
   if (!choices)
   {
      cJSON_Delete(root);
      return -1;
   }
   cJSON_AddItemToObject(root, "choices", choices);

   cJSON *choice = cJSON_CreateObject();
   if (!choice)
   {
      cJSON_Delete(root);
      return -1;
   }
   cJSON_AddNumberToObject(choice, "index", 0.0);
   cJSON_AddItemToArray(choices, choice);

   cJSON *msg = cJSON_CreateObject();
   if (!msg)
   {
      cJSON_Delete(root);
      return -1;
   }
   cJSON_AddStringToObject(msg, "role", "assistant");
   cJSON_AddStringToObject(msg, "content", content ? content : "");
   cJSON_AddItemToObject(choice, "message", msg);

   cJSON_AddStringToObject(choice, "finish_reason", "stop");

   cJSON *usage = cJSON_CreateObject();
   if (!usage)
   {
      cJSON_Delete(root);
      return -1;
   }
   cJSON_AddItemToObject(root, "usage", usage);
   cJSON_AddNumberToObject(usage, "prompt_tokens", (double)prompt_tokens);
   cJSON_AddNumberToObject(usage, "completion_tokens", (double)completion_tokens);
   cJSON_AddNumberToObject(usage, "total_tokens", (double)(prompt_tokens + completion_tokens));

   char *s = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!s)
      return -1;

   int len = snprintf(resp, (size_t)cap, "%s", s);
   free(s);
   return (len < 0 || len >= cap) ? -1 : len;
}

/* ── openai_format_chat_chunk (streaming SSE delta) ──────────────────────── */

int openai_format_chat_chunk(const char *id, const char *model, long created, int role,
                             const char *delta_content, int finish, char *resp, int cap)
{
   if (!resp || cap <= 0)
      return -1;

   cJSON *root = cJSON_CreateObject();
   if (!root)
      return -1;

   cJSON_AddStringToObject(root, "id", id ? id : "");
   cJSON_AddStringToObject(root, "object", "chat.completion.chunk");
   cJSON_AddNumberToObject(root, "created", (double)created);
   cJSON_AddStringToObject(root, "model", model ? model : "");

   cJSON *choices = cJSON_CreateArray();
   cJSON *choice = cJSON_CreateObject();
   cJSON *delta = cJSON_CreateObject();
   if (!choices || !choice || !delta)
   {
      cJSON_Delete(root);
      cJSON_Delete(choices);
      cJSON_Delete(choice);
      cJSON_Delete(delta);
      return -1;
   }
   cJSON_AddItemToObject(root, "choices", choices);
   cJSON_AddItemToArray(choices, choice);
   cJSON_AddNumberToObject(choice, "index", 0.0);
   cJSON_AddItemToObject(choice, "delta", delta);

   /* The first frame announces the assistant role; content frames carry the
    * text delta; the terminal frame sets finish_reason and an empty delta. */
   if (role)
      cJSON_AddStringToObject(delta, "role", "assistant");
   if (delta_content)
      cJSON_AddStringToObject(delta, "content", delta_content);

   if (finish)
      cJSON_AddStringToObject(choice, "finish_reason", "stop");
   else
      cJSON_AddNullToObject(choice, "finish_reason");

   char *s = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!s)
      return -1;
   int len = snprintf(resp, (size_t)cap, "%s", s);
   free(s);
   return (len < 0 || len >= cap) ? -1 : len;
}

/* ── openai_format_text_chunk (legacy completions streaming) ─────────────── */

int openai_format_text_chunk(const char *id, const char *model, long created,
                             const char *text_delta, int finish, char *resp, int cap)
{
   if (!resp || cap <= 0)
      return -1;

   cJSON *root = cJSON_CreateObject();
   if (!root)
      return -1;

   cJSON_AddStringToObject(root, "id", id ? id : "");
   cJSON_AddStringToObject(root, "object", "text_completion");
   cJSON_AddNumberToObject(root, "created", (double)created);
   cJSON_AddStringToObject(root, "model", model ? model : "");

   cJSON *choices = cJSON_CreateArray();
   cJSON *choice = cJSON_CreateObject();
   if (!choices || !choice)
   {
      cJSON_Delete(root);
      cJSON_Delete(choices);
      cJSON_Delete(choice);
      return -1;
   }
   cJSON_AddItemToObject(root, "choices", choices);
   cJSON_AddItemToArray(choices, choice);
   cJSON_AddStringToObject(choice, "text", text_delta ? text_delta : "");
   cJSON_AddNumberToObject(choice, "index", 0.0);
   if (finish)
      cJSON_AddStringToObject(choice, "finish_reason", "stop");
   else
      cJSON_AddNullToObject(choice, "finish_reason");

   char *s = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!s)
      return -1;
   int len = snprintf(resp, (size_t)cap, "%s", s);
   free(s);
   return (len < 0 || len >= cap) ? -1 : len;
}

/* ── openai_format_text_completion ───────────────────────────────────── */

int openai_format_text_completion(const char *id, const char *model, const char *content,
                                  long created, int prompt_tokens, int completion_tokens,
                                  char *resp, int cap)
{
   if (!resp || cap <= 0)
      return -1;

   cJSON *root = cJSON_CreateObject();
   if (!root)
      return -1;

   cJSON_AddStringToObject(root, "id", id ? id : "");
   cJSON_AddStringToObject(root, "object", "text_completion");
   cJSON_AddNumberToObject(root, "created", (double)created);
   cJSON_AddStringToObject(root, "model", model ? model : "");

   cJSON *choices = cJSON_CreateArray();
   if (!choices)
   {
      cJSON_Delete(root);
      return -1;
   }
   cJSON_AddItemToObject(root, "choices", choices);

   cJSON *choice = cJSON_CreateObject();
   if (!choice)
   {
      cJSON_Delete(root);
      return -1;
   }
   cJSON_AddNumberToObject(choice, "index", 0.0);
   cJSON_AddItemToArray(choices, choice);

   cJSON_AddStringToObject(choice, "text", content ? content : "");
   cJSON_AddStringToObject(choice, "finish_reason", "stop");

   cJSON *usage = cJSON_CreateObject();
   if (!usage)
   {
      cJSON_Delete(root);
      return -1;
   }
   cJSON_AddItemToObject(root, "usage", usage);
   cJSON_AddNumberToObject(usage, "prompt_tokens", (double)prompt_tokens);
   cJSON_AddNumberToObject(usage, "completion_tokens", (double)completion_tokens);
   cJSON_AddNumberToObject(usage, "total_tokens", (double)(prompt_tokens + completion_tokens));

   char *s = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!s)
      return -1;

   int len = snprintf(resp, (size_t)cap, "%s", s);
   free(s);
   return (len < 0 || len >= cap) ? -1 : len;
}

/* ── openai_format_response (Responses API) ──────────────────────────────── */

/* Build a `response` object (caller owns the returned cJSON, or NULL on OOM).
 * status is e.g. "completed" or "in_progress". When with_output != 0 the
 * assistant output message + content part + usage are included; otherwise the
 * output array is empty and usage is omitted (the shape of an in-progress
 * response just after creation). */
static cJSON *build_response_object(const char *id, const char *model, const char *output_text,
                                    long created, int prompt_tokens, int completion_tokens,
                                    const char *status, int with_output, const char *object_type)
{
   cJSON *root = cJSON_CreateObject();
   if (!root)
      return NULL;

   cJSON_AddStringToObject(root, "id", id ? id : "");
   cJSON_AddStringToObject(root, "object", object_type ? object_type : "response");
   cJSON_AddNumberToObject(root, "created_at", (double)created);
   cJSON_AddStringToObject(root, "model", model ? model : "");
   cJSON_AddStringToObject(root, "status", status ? status : "completed");

   cJSON *output = cJSON_AddArrayToObject(root, "output");
   if (!output)
   {
      cJSON_Delete(root);
      return NULL;
   }
   if (!with_output)
      return root;

   cJSON *msg = cJSON_CreateObject();
   if (!msg)
   {
      cJSON_Delete(root);
      return NULL;
   }
   cJSON_AddItemToArray(output, msg);
   char msg_id[64];
   snprintf(msg_id, sizeof(msg_id), "%s-msg", id ? id : "resp");
   cJSON_AddStringToObject(msg, "id", msg_id);
   cJSON_AddStringToObject(msg, "type", "message");
   cJSON_AddStringToObject(msg, "status", "completed");
   cJSON_AddStringToObject(msg, "role", "assistant");

   cJSON *content = cJSON_AddArrayToObject(msg, "content");
   cJSON *part = cJSON_CreateObject();
   if (!content || !part)
   {
      cJSON_Delete(part);
      cJSON_Delete(root);
      return NULL;
   }
   cJSON_AddItemToArray(content, part);
   cJSON_AddStringToObject(part, "type", "output_text");
   cJSON_AddStringToObject(part, "text", output_text ? output_text : "");
   cJSON_AddItemToObject(part, "annotations", cJSON_CreateArray());

   cJSON *usage = cJSON_AddObjectToObject(root, "usage");
   if (usage)
   {
      cJSON_AddNumberToObject(usage, "input_tokens", (double)prompt_tokens);
      cJSON_AddNumberToObject(usage, "output_tokens", (double)completion_tokens);
      cJSON_AddNumberToObject(usage, "total_tokens", (double)(prompt_tokens + completion_tokens));
   }
   return root;
}

/* Print a (possibly wrapped) cJSON object into resp[cap], deleting it. */
static int emit_json(cJSON *root, char *resp, int cap)
{
   if (!root)
      return -1;
   char *s = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!s)
      return -1;
   int len = snprintf(resp, (size_t)cap, "%s", s);
   free(s);
   return (len < 0 || len >= cap) ? -1 : len;
}

int openai_format_response(const char *id, const char *model, const char *output_text, long created,
                           int prompt_tokens, int completion_tokens, char *resp, int cap)
{
   if (!resp || cap <= 0)
      return -1;
   cJSON *root = build_response_object(id, model, output_text, created, prompt_tokens,
                                       completion_tokens, "completed", 1, "response");
   return emit_json(root, resp, cap);
}

int openai_format_run(const char *id, const char *model, const char *output_text, long created,
                      int prompt_tokens, int completion_tokens, const char *status, char *resp,
                      int cap)
{
   if (!resp || cap <= 0)
      return -1;
   cJSON *root = build_response_object(id, model, output_text, created, prompt_tokens,
                                       completion_tokens, status ? status : "completed", 1, "run");
   return emit_json(root, resp, cap);
}

/* ── Responses API streaming events ──────────────────────────────────────── */

int openai_format_responses_created(const char *id, const char *model, long created, char *resp,
                                    int cap)
{
   if (!resp || cap <= 0)
      return -1;
   cJSON *root = cJSON_CreateObject();
   if (!root)
      return -1;
   cJSON_AddStringToObject(root, "type", "response.created");
   cJSON *obj = build_response_object(id, model, "", created, 0, 0, "in_progress", 0, "response");
   if (!obj)
   {
      cJSON_Delete(root);
      return -1;
   }
   cJSON_AddItemToObject(root, "response", obj);
   return emit_json(root, resp, cap);
}

int openai_format_responses_delta(const char *item_id, const char *delta, char *resp, int cap)
{
   if (!resp || cap <= 0)
      return -1;
   cJSON *root = cJSON_CreateObject();
   if (!root)
      return -1;
   cJSON_AddStringToObject(root, "type", "response.output_text.delta");
   cJSON_AddStringToObject(root, "item_id", item_id ? item_id : "");
   cJSON_AddNumberToObject(root, "output_index", 0.0);
   cJSON_AddNumberToObject(root, "content_index", 0.0);
   cJSON_AddStringToObject(root, "delta", delta ? delta : "");
   return emit_json(root, resp, cap);
}

int openai_format_responses_completed(const char *id, const char *model, const char *output_text,
                                      long created, int prompt_tokens, int completion_tokens,
                                      char *resp, int cap)
{
   if (!resp || cap <= 0)
      return -1;
   cJSON *root = cJSON_CreateObject();
   if (!root)
      return -1;
   cJSON_AddStringToObject(root, "type", "response.completed");
   cJSON *obj = build_response_object(id, model, output_text, created, prompt_tokens,
                                      completion_tokens, "completed", 1, "response");
   if (!obj)
   {
      cJSON_Delete(root);
      return -1;
   }
   cJSON_AddItemToObject(root, "response", obj);
   return emit_json(root, resp, cap);
}

/* ── Responses API output items (Codex parity) ──────────────────────────────
 *
 * Codex's Responses-API parser requires an *active output item* to exist before
 * any text/arguments delta — a bare created→delta→completed stream is rejected
 * ("OutputTextDelta without active item") and the text is dropped. These helpers
 * build the message / function_call output items and the output_item.added/.done
 * and function_call_arguments.delta/.done events that wrap a turn. */

cJSON *openai_responses_message_item(const char *item_id, const char *text, const char *status)
{
   cJSON *item = cJSON_CreateObject();
   if (!item)
      return NULL;
   cJSON_AddStringToObject(item, "id", item_id ? item_id : "");
   cJSON_AddStringToObject(item, "type", "message");
   cJSON_AddStringToObject(item, "status", status ? status : "completed");
   cJSON_AddStringToObject(item, "role", "assistant");
   cJSON *content = cJSON_AddArrayToObject(item, "content");
   if (content && text && text[0])
   {
      cJSON *part = cJSON_CreateObject();
      if (part)
      {
         cJSON_AddStringToObject(part, "type", "output_text");
         cJSON_AddStringToObject(part, "text", text);
         cJSON_AddItemToObject(part, "annotations", cJSON_CreateArray());
         cJSON_AddItemToArray(content, part);
      }
   }
   return item;
}

cJSON *openai_responses_function_call_item(const char *item_id, const char *call_id,
                                           const char *name, const char *arguments,
                                           const char *status)
{
   cJSON *item = cJSON_CreateObject();
   if (!item)
      return NULL;
   cJSON_AddStringToObject(item, "id", item_id ? item_id : "");
   cJSON_AddStringToObject(item, "type", "function_call");
   cJSON_AddStringToObject(item, "status", status ? status : "completed");
   cJSON_AddStringToObject(item, "name", name ? name : "");
   cJSON_AddStringToObject(item, "call_id", call_id ? call_id : "");
   cJSON_AddStringToObject(item, "arguments", arguments ? arguments : "");
   return item;
}

/* Wrap a (consumed) item object in a response.output_item.added/.done event. */
static int emit_output_item_event(const char *type, int output_index, cJSON *item, char *resp,
                                  int cap)
{
   if (!item)
      return -1;
   cJSON *root = cJSON_CreateObject();
   if (!root)
   {
      cJSON_Delete(item);
      return -1;
   }
   cJSON_AddStringToObject(root, "type", type);
   cJSON_AddNumberToObject(root, "output_index", (double)output_index);
   cJSON_AddItemToObject(root, "item", item);
   return emit_json(root, resp, cap);
}

int openai_format_responses_msg_item_added(const char *item_id, int output_index, char *resp,
                                           int cap)
{
   if (!resp || cap <= 0)
      return -1;
   return emit_output_item_event("response.output_item.added", output_index,
                                 openai_responses_message_item(item_id, "", "in_progress"), resp,
                                 cap);
}

int openai_format_responses_msg_item_done(const char *item_id, const char *text, int output_index,
                                          char *resp, int cap)
{
   if (!resp || cap <= 0)
      return -1;
   return emit_output_item_event("response.output_item.done", output_index,
                                 openai_responses_message_item(item_id, text, "completed"), resp,
                                 cap);
}

int openai_format_responses_fc_item_added(const char *item_id, const char *call_id,
                                          const char *name, int output_index, char *resp, int cap)
{
   if (!resp || cap <= 0)
      return -1;
   return emit_output_item_event(
       "response.output_item.added", output_index,
       openai_responses_function_call_item(item_id, call_id, name, "", "in_progress"), resp, cap);
}

int openai_format_responses_fc_item_done(const char *item_id, const char *call_id, const char *name,
                                         const char *arguments, int output_index, char *resp,
                                         int cap)
{
   if (!resp || cap <= 0)
      return -1;
   return emit_output_item_event(
       "response.output_item.done", output_index,
       openai_responses_function_call_item(item_id, call_id, name, arguments, "completed"), resp,
       cap);
}

int openai_format_responses_fc_args_delta(const char *item_id, int output_index, const char *delta,
                                          char *resp, int cap)
{
   if (!resp || cap <= 0)
      return -1;
   cJSON *root = cJSON_CreateObject();
   if (!root)
      return -1;
   cJSON_AddStringToObject(root, "type", "response.function_call_arguments.delta");
   cJSON_AddStringToObject(root, "item_id", item_id ? item_id : "");
   cJSON_AddNumberToObject(root, "output_index", (double)output_index);
   cJSON_AddStringToObject(root, "delta", delta ? delta : "");
   return emit_json(root, resp, cap);
}

int openai_format_responses_fc_args_done(const char *item_id, int output_index,
                                         const char *arguments, char *resp, int cap)
{
   if (!resp || cap <= 0)
      return -1;
   cJSON *root = cJSON_CreateObject();
   if (!root)
      return -1;
   cJSON_AddStringToObject(root, "type", "response.function_call_arguments.done");
   cJSON_AddStringToObject(root, "item_id", item_id ? item_id : "");
   cJSON_AddNumberToObject(root, "output_index", (double)output_index);
   cJSON_AddStringToObject(root, "arguments", arguments ? arguments : "");
   return emit_json(root, resp, cap);
}

/* response.completed carrying a caller-built output array (consumed). Used for
 * function_call turns where `output` holds function_call items rather than a
 * message. */
int openai_format_responses_completed_items(const char *id, const char *model, long created,
                                            cJSON *output_arr, int prompt_tokens,
                                            int completion_tokens, char *resp, int cap)
{
   if (!resp || cap <= 0)
   {
      cJSON_Delete(output_arr);
      return -1;
   }
   cJSON *root = cJSON_CreateObject();
   if (!root)
   {
      cJSON_Delete(output_arr);
      return -1;
   }
   cJSON_AddStringToObject(root, "type", "response.completed");
   cJSON *r = cJSON_CreateObject();
   if (!r)
   {
      cJSON_Delete(output_arr);
      cJSON_Delete(root);
      return -1;
   }
   cJSON_AddItemToObject(root, "response", r);
   cJSON_AddStringToObject(r, "id", id ? id : "");
   cJSON_AddStringToObject(r, "object", "response");
   cJSON_AddNumberToObject(r, "created_at", (double)created);
   cJSON_AddStringToObject(r, "model", model ? model : "");
   cJSON_AddStringToObject(r, "status", "completed");
   cJSON_AddItemToObject(r, "output", output_arr ? output_arr : cJSON_CreateArray());
   cJSON *usage = cJSON_AddObjectToObject(r, "usage");
   if (usage)
   {
      cJSON_AddNumberToObject(usage, "input_tokens", (double)prompt_tokens);
      cJSON_AddNumberToObject(usage, "output_tokens", (double)completion_tokens);
      cJSON_AddNumberToObject(usage, "total_tokens", (double)(prompt_tokens + completion_tokens));
   }
   return emit_json(root, resp, cap);
}

/* ── openai_format_error ──────────────────────────────────────────────── */

int openai_format_error(char *resp, int cap, const char *type, const char *message)
{
   if (!resp || cap <= 0)
      return -1;

   cJSON *root = cJSON_CreateObject();
   if (!root)
      return -1;

   cJSON *err = cJSON_CreateObject();
   if (!err)
   {
      cJSON_Delete(root);
      return -1;
   }
   cJSON_AddItemToObject(root, "error", err);
   cJSON_AddStringToObject(err, "message", message ? message : "");
   cJSON_AddStringToObject(err, "type", type ? type : "");

   char *s = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!s)
      return -1;

   int len = snprintf(resp, (size_t)cap, "%s", s);
   free(s);
   return (len < 0 || len >= cap) ? -1 : len;
}