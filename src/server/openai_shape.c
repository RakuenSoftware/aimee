/* openai_shape.c: OpenAI-compatible JSON shaping helpers (no sockets, no network). */
#include "openai_shape.h"
#include "cJSON.h"
#include "aimee.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void openai_upstream_error_message(int status, const char *body, char *out, size_t cap)
{
   if (!out || !cap)
      return;
   if (status <= 0)
   {
      snprintf(out, cap, "upstream model transport failed");
      return;
   }
   cJSON *root = body ? cJSON_Parse(body) : NULL;
   const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
   const char *message = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "message"));
   if (!message || !message[0])
      message = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "detail"));
   if (message && message[0])
      snprintf(out, cap, "upstream model HTTP %d: %.400s", status, message);
   else
      snprintf(out, cap, "upstream model HTTP %d: no usable error detail", status);
   /* Provider diagnostics can contain terminal control bytes. Preserve readable
    * text, but do not allow them to control clients' terminals or log framing. */
   for (char *p = out; *p; p++)
      if ((unsigned char)*p < 32 || (unsigned char)*p == 127)
         *p = ' ';
   cJSON_Delete(root);
}

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

/* Optional streaming flag used by HTTP routing. */

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

/* ── openai_format_chat_chunk_finish ─────────────────────────────────────── */

int openai_format_chat_chunk_finish(const char *id, const char *model, long created,
                                    const char *reason, char *resp, int cap)
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

   cJSON *choices = cJSON_AddArrayToObject(root, "choices");
   cJSON *choice = cJSON_CreateObject();
   if (!choices || !choice)
   {
      cJSON_Delete(choice);
      cJSON_Delete(root);
      return -1;
   }
   cJSON_AddNumberToObject(choice, "index", 0.0);
   cJSON_AddItemToArray(choices, choice);
   cJSON_AddItemToObject(choice, "delta", cJSON_CreateObject());
   cJSON_AddStringToObject(choice, "finish_reason", reason ? reason : "stop");

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
                                  int usage_unobserved, char *resp, int cap)
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

   if (!usage_unobserved)
   {
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
   }

   char *s = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!s)
      return -1;

   int len = snprintf(resp, (size_t)cap, "%s", s);
   free(s);
   return (len < 0 || len >= cap) ? -1 : len;
}

/* Empty response envelope shared by the active created and failed events. */
static cJSON *build_response_object(const char *id, const char *model, long created,
                                    const char *status)
{
   cJSON *root = cJSON_CreateObject();
   if (!root)
      return NULL;

   cJSON_AddStringToObject(root, "id", id ? id : "");
   cJSON_AddStringToObject(root, "object", "response");
   cJSON_AddNumberToObject(root, "created_at", (double)created);
   cJSON_AddStringToObject(root, "model", model ? model : "");
   cJSON_AddStringToObject(root, "status", status);

   cJSON *output = cJSON_AddArrayToObject(root, "output");
   if (!output)
   {
      cJSON_Delete(root);
      return NULL;
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
   cJSON *obj = build_response_object(id, model, created, "in_progress");
   if (!obj)
   {
      cJSON_Delete(root);
      return -1;
   }
   cJSON_AddItemToObject(root, "response", obj);
   return emit_json(root, resp, cap);
}

int openai_format_responses_failed(const char *id, const char *model, long created,
                                   const char *code, const char *message, char *resp, int cap)
{
   if (!resp || cap <= 0)
      return -1;
   cJSON *root = cJSON_CreateObject();
   if (!root)
      return -1;
   cJSON_AddStringToObject(root, "type", "response.failed");
   /* Base response object (output:[] is fine; Codex reads only response.error). */
   cJSON *obj = build_response_object(id, model, created, "failed");
   if (!obj)
   {
      cJSON_Delete(root);
      return -1;
   }
   cJSON *err = cJSON_AddObjectToObject(obj, "error");
   if (err)
   {
      cJSON_AddStringToObject(err, "code", code ? code : "server_error");
      cJSON_AddStringToObject(err, "message", message ? message : "");
   }
   cJSON_AddItemToObject(root, "response", obj);
   return emit_json(root, resp, cap);
}

/* Wrap a (consumed) item object in a response.output_item.added/.done event. */
/* response.completed carrying a caller-built output array (consumed). Used for
 * function_call turns where `output` holds function_call items rather than a
 * message. */
/* ── openai_format_error ──────────────────────────────────────────────── */

int openai_format_error(char *resp, int cap, const char *type, const char *message)
{
   return openai_format_error_code(resp, cap, type, message, 0);
}

int openai_format_error_code(char *resp, int cap, const char *type, const char *message,
                             int aimee_code)
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
   /* aimee-internal faults stamp their code (>=1000) into the body; the caller
    * logs it (this formatter stays a pure, dependency-light shape helper). */
   if (aimee_code > 0)
      cJSON_AddNumberToObject(err, "code", aimee_code);

   char *s = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!s)
      return -1;

   int len = snprintf(resp, (size_t)cap, "%s", s);
   free(s);
   return (len < 0 || len >= cap) ? -1 : len;
}
