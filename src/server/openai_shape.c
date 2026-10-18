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
   return openai_format_chat_completion_reason(id, model, content, created, prompt_tokens,
                                               completion_tokens, NULL, resp, cap);
}

int openai_format_chat_completion_reason(const char *id, const char *model, const char *content,
                                         long created, int prompt_tokens, int completion_tokens,
                                         const char *finish_reason, char *resp, int cap)
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

   cJSON_AddStringToObject(choice, "finish_reason",
                           finish_reason && finish_reason[0] ? finish_reason : "stop");

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

/* Attach a Responses-shaped `usage` block, INCLUDING the cached-input detail.
 *
 * cached_tokens is the portion of prompt_tokens the provider served from its
 * prompt cache. It is NOT a flat counter: OpenAI reports it in a sibling object
 * (`input_tokens_details.cached_tokens`), and a usage block that enumerates only
 * the flat fields drops it silently -- the field is not missing from the input,
 * it is missing from the enumeration, so nothing errors and no test that asserts
 * the flat counters notices.
 *
 * That is not hypothetical: it is why every benchmark cell run through this
 * gateway reported cache_read=0 while every other arm reported 89-95%. The
 * client (Codex) can only bill what we hand it, so an omitted field reads as
 * "prompt caching never worked" and prices the whole conversation at the
 * uncached rate. Emitted unconditionally, even at zero, so a client can tell a
 * genuine cache miss from a build that does not report caching at all. */
static void add_responses_usage(cJSON *parent, int prompt_tokens, int completion_tokens,
                                int cached_tokens)
{
   cJSON *usage = cJSON_AddObjectToObject(parent, "usage");
   if (!usage)
      return;
   cJSON_AddNumberToObject(usage, "input_tokens", (double)prompt_tokens);
   cJSON_AddNumberToObject(usage, "output_tokens", (double)completion_tokens);
   cJSON_AddNumberToObject(usage, "total_tokens", (double)(prompt_tokens + completion_tokens));
   cJSON *details = cJSON_AddObjectToObject(usage, "input_tokens_details");
   if (details)
      cJSON_AddNumberToObject(details, "cached_tokens", (double)cached_tokens);
}

/* Build a `response` object (caller owns the returned cJSON, or NULL on OOM).
 * status is e.g. "completed" or "in_progress". When with_output != 0 the
 * assistant output message + content part + usage are included; otherwise the
 * output array is empty and usage is omitted (the shape of an in-progress
 * response just after creation). */
static cJSON *build_response_object(const char *id, const char *model, const char *output_text,
                                    long created, int prompt_tokens, int completion_tokens,
                                    int cached_tokens, const char *status, int with_output,
                                    const char *object_type)
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

   add_responses_usage(root, prompt_tokens, completion_tokens, cached_tokens);
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
                           int prompt_tokens, int completion_tokens, int cached_tokens, char *resp,
                           int cap)
{
   return openai_format_response_status(id, model, output_text, created, prompt_tokens,
                                        completion_tokens, cached_tokens, NULL, resp, cap);
}

int openai_format_response_status(const char *id, const char *model, const char *output_text,
                                  long created, int prompt_tokens, int completion_tokens,
                                  int cached_tokens, const char *incomplete_reason, char *resp,
                                  int cap)
{
   if (!resp || cap <= 0)
      return -1;
   cJSON *root = build_response_object(
       id, model, output_text, created, prompt_tokens, completion_tokens, cached_tokens,
       incomplete_reason ? "incomplete" : "completed", 1, "response");
   if (root && incomplete_reason)
   {
      cJSON *det = cJSON_AddObjectToObject(root, "incomplete_details");
      if (det)
         cJSON_AddStringToObject(det, "reason", incomplete_reason);
   }
   return emit_json(root, resp, cap);
}

int openai_format_run(const char *id, const char *model, const char *output_text, long created,
                      int prompt_tokens, int completion_tokens, int cached_tokens,
                      const char *status, char *resp, int cap)
{
   if (!resp || cap <= 0)
      return -1;
   cJSON *root =
       build_response_object(id, model, output_text, created, prompt_tokens, completion_tokens,
                             cached_tokens, status ? status : "completed", 1, "run");
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
   cJSON *obj =
       build_response_object(id, model, "", created, 0, 0, 0, "in_progress", 0, "response");
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
                                      int cached_tokens, char *resp, int cap)
{
   if (!resp || cap <= 0)
      return -1;
   cJSON *root = cJSON_CreateObject();
   if (!root)
      return -1;
   cJSON_AddStringToObject(root, "type", "response.completed");
   cJSON *obj = build_response_object(id, model, output_text, created, prompt_tokens,
                                      completion_tokens, cached_tokens, "completed", 1, "response");
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
   cJSON *obj = build_response_object(id, model, "", created, 0, 0, 0, "failed", 0, "response");
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

int openai_format_responses_incomplete(const char *id, const char *model, long created,
                                       const char *reason, char *resp, int cap)
{
   if (!resp || cap <= 0)
      return -1;
   cJSON *root = cJSON_CreateObject();
   if (!root)
      return -1;
   cJSON_AddStringToObject(root, "type", "response.incomplete");
   cJSON *obj = build_response_object(id, model, "", created, 0, 0, 0, "incomplete", 0, "response");
   if (!obj)
   {
      cJSON_Delete(root);
      return -1;
   }
   cJSON *det = cJSON_AddObjectToObject(obj, "incomplete_details");
   if (det)
      cJSON_AddStringToObject(det, "reason", reason ? reason : "unknown");
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
                                           const char *name, const char *tool_namespace,
                                           const char *arguments, const char *status)
{
   cJSON *item = cJSON_CreateObject();
   if (!item)
      return NULL;
   cJSON_AddStringToObject(item, "id", item_id ? item_id : "");
   cJSON_AddStringToObject(item, "type", "function_call");
   cJSON_AddStringToObject(item, "status", status ? status : "completed");
   cJSON_AddStringToObject(item, "name", name ? name : "");
   /* Only when the call actually has one: a plain tool carries no `namespace`, and
    * emitting an empty one would claim a group that does not exist. */
   if (tool_namespace && tool_namespace[0])
      cJSON_AddStringToObject(item, "namespace", tool_namespace);
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
                                          const char *name, const char *tool_namespace,
                                          int output_index, char *resp, int cap)
{
   if (!resp || cap <= 0)
      return -1;
   return emit_output_item_event("response.output_item.added", output_index,
                                 openai_responses_function_call_item(
                                     item_id, call_id, name, tool_namespace, "", "in_progress"),
                                 resp, cap);
}

int openai_format_responses_fc_item_done(const char *item_id, const char *call_id, const char *name,
                                         const char *tool_namespace, const char *arguments,
                                         int output_index, char *resp, int cap)
{
   if (!resp || cap <= 0)
      return -1;
   return emit_output_item_event("response.output_item.done", output_index,
                                 openai_responses_function_call_item(item_id, call_id, name,
                                                                     tool_namespace, arguments,
                                                                     "completed"),
                                 resp, cap);
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
                                            int completion_tokens, int cached_tokens, char *resp,
                                            int cap)
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
   add_responses_usage(r, prompt_tokens, completion_tokens, cached_tokens);
   return emit_json(root, resp, cap);
}

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
