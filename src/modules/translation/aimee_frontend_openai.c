/* OpenAI Chat request admission and canonical Chat/Responses client output.
 * Native JSON/SSE is rendered only at these translation boundaries. */
#include <aimee/translation/aimee_frontend.h>

#include "cJSON.h"

#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static char *dupstr(const char *s)
{
   return s ? strdup(s) : NULL;
}

int openai_family_prompt_cache_parse(const cJSON *native, aimee_request_t *out, char *error,
                                     size_t capacity)
{
   if (!out || !cJSON_IsObject(native) || !aimee_ir_json_unique(native))
      return -1;
   static const char *const names[] = {"prompt_cache_key", "prompt_cache_retention"};
   char **values[] = {&out->prompt_cache_key, &out->prompt_cache_retention};
   for (int i = 0; i < 2; i++)
   {
      const cJSON *value = cJSON_GetObjectItemCaseSensitive(native, names[i]);
      if (!value || cJSON_IsNull(value))
         continue;
      if (!cJSON_IsString(value) || !(*values[i] = strdup(value->valuestring)))
         goto fail;
   }
   const cJSON *options = cJSON_GetObjectItemCaseSensitive(native, "prompt_cache_options");
   if (options && !cJSON_IsNull(options))
   {
      if (!cJSON_IsObject(options) || !(out->prompt_cache_options = cJSON_Duplicate(options, 1)))
         goto fail;
   }
   return 0;
fail:
   if (error && capacity)
      snprintf(error, capacity, "invalid OpenAI prompt cache configuration");
   return -1;
}

static const char *ostr(const cJSON *o, const char *k)
{
   const cJSON *it = cJSON_GetObjectItemCaseSensitive((cJSON *)o, k);
   return (it && cJSON_IsString(it)) ? it->valuestring : NULL;
}

/* grow `*arr` (of element size `esz`) by one, return the new (zeroed) slot or NULL. */
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

/* Parse an OpenAI content value (string OR array of parts) and APPEND blocks to
 * (*blocks,*n). type:"text" -> TEXT; type:"image_url" -> IMAGE (media_ref=url). */
int openai_ir_parse_content(const cJSON *content, aimee_block_t **blocks, int *n)
{
   if (!content)
      return 0;
   if (cJSON_IsString(content))
   {
      aimee_block_t *b = grow1((void **)blocks, n, sizeof(aimee_block_t));
      if (!b)
         return -1;
      b->type = AIMEE_BLK_TEXT;
      b->text = dupstr(content->valuestring);
      return 0;
   }
   if (!cJSON_IsArray(content))
      return 0;
   const cJSON *el = NULL;
   cJSON_ArrayForEach(el, content)
   {
      aimee_block_t *b = grow1((void **)blocks, n, sizeof(aimee_block_t));
      if (!b)
         return -1;
      b->raw = cJSON_Duplicate(el, 1);
      const char *type = ostr(el, "type");
      if (type && strcmp(type, "image_url") == 0)
      {
         b->type = AIMEE_BLK_IMAGE;
         const cJSON *iu = cJSON_GetObjectItemCaseSensitive((cJSON *)el, "image_url");
         const char *url = iu ? ostr(iu, "url") : NULL;
/* Canonicalize an inline base64 data: URL into the structured media_type + base64
 * data form the Anthropic egress emits, so an OpenAI-sourced inline image serializes
 * identically to an Anthropic-sourced one. The RFC-2397 shape is
 * "data:<media_type>;base64,<data>": the FIRST comma delimits the header from the
 * data, and the header must end with ";base64". Anything else (plain http(s) URL, a
 * non-base64 data URL, or a ";base64," that appears only inside the payload after the
 * comma) is kept as a url reference verbatim -- Anthropic has no data:-url image form,
 * so there is no cross-protocol counterpart to canonicalize toward. */
#define DATA_URL_PREFIX_LEN 5 /* strlen("data:") */
#define B64_SUFFIX          ";base64"
#define B64_SUFFIX_LEN      7 /* strlen(";base64") */
         const char *comma = url ? strchr(url, ',') : NULL;
         int decomposed = 0;
         if (url && strncmp(url, "data:", DATA_URL_PREFIX_LEN) == 0 && comma)
         {
            const char *hdr = url + DATA_URL_PREFIX_LEN;
            size_t hdr_len = (size_t)(comma - hdr);
            if (hdr_len > B64_SUFFIX_LEN &&
                strncmp(comma - B64_SUFFIX_LEN, B64_SUFFIX, B64_SUFFIX_LEN) == 0)
            {
               size_t mt_len = hdr_len - B64_SUFFIX_LEN; /* media_type before ";base64" */
               b->media_type = strndup(hdr, mt_len);
               b->media_ref = strdup(comma + 1);
               decomposed = 1;
            }
         }
         if (!decomposed)
            b->media_ref = dupstr(url);
#undef DATA_URL_PREFIX_LEN
#undef B64_SUFFIX
#undef B64_SUFFIX_LEN
      }
      else if (type && !strcmp(type, "file"))
      {
         b->type = AIMEE_BLK_DOCUMENT;
         const cJSON *file = cJSON_GetObjectItemCaseSensitive(el, "file");
         const char *ref = ostr(file, "file_id");
         b->media_ref = dupstr(ref ? ref : ostr(file, "file_data"));
      }
      else if (type && (!strcmp(type, "text") || !strcmp(type, "output_text")))
      {
         b->type = AIMEE_BLK_TEXT;
         b->text = dupstr(ostr(el, "text"));
      }
      else
         b->type = AIMEE_BLK_UNKNOWN;
   }
   return 0;
}

int openai_family_output_format_parse(const cJSON *format, int responses_shape,
                                      aimee_request_t *out, char *error, size_t capacity)
{
   if (!format || cJSON_IsNull(format))
      return 0;
   const char *type = ostr(format, "type");
   if (!type)
      goto invalid;
   aimee_output_format_t *output = &out->output_format;
   if (!strcmp(type, "text"))
      output->kind = AIMEE_OUTPUT_TEXT;
   else if (!strcmp(type, "json_object"))
      output->kind = AIMEE_OUTPUT_JSON_OBJECT;
   else if (!strcmp(type, "json_schema"))
   {
      output->kind = AIMEE_OUTPUT_JSON_SCHEMA;
      const cJSON *definition =
          responses_shape ? format : cJSON_GetObjectItemCaseSensitive(format, "json_schema");
      const char *name = ostr(definition, "name");
      const cJSON *schema = cJSON_GetObjectItemCaseSensitive(definition, "schema");
      const cJSON *description = cJSON_GetObjectItemCaseSensitive(definition, "description");
      const cJSON *strict = cJSON_GetObjectItemCaseSensitive(definition, "strict");
      if (!name || !name[0] || strlen(name) > 64 ||
          strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
              strlen(name) ||
          !cJSON_IsObject(schema) || (description && !cJSON_IsString(description)) ||
          (strict && !cJSON_IsNull(strict) && !cJSON_IsBool(strict)))
         goto invalid;
      output->name = dupstr(name);
      output->schema = cJSON_Duplicate(schema, 1);
      output->description = description ? dupstr(description->valuestring) : NULL;
      output->strict = !strict                ? AIMEE_STRICT_UNSET
                       : cJSON_IsNull(strict) ? AIMEE_STRICT_NULL
                       : cJSON_IsTrue(strict) ? AIMEE_STRICT_TRUE
                                              : AIMEE_STRICT_FALSE;
      if (!output->name || !output->schema || (description && !output->description))
         goto invalid;
   }
   else
      goto invalid;
   return 0;
invalid:
   if (error && capacity)
      snprintf(error, capacity, "invalid or unsupported structured output format");
   return -1;
}

int openai_family_tool_choice_parse(const cJSON *tc, const cJSON *parallel, int responses_shape,
                                    aimee_request_t *out, char *err, size_t errn)
{
   if (cJSON_IsBool(parallel))
   {
      out->has_parallel_tool_calls = 1;
      out->parallel_tool_calls = cJSON_IsTrue(parallel) ? 1 : 0;
   }
   if (!tc || cJSON_IsNull(tc))
      return 0;
   if (cJSON_IsString(tc))
   {
      const char *v = tc->valuestring;
      if (strcmp(v, "auto") == 0)
         out->tool_choice = AIMEE_TOOL_CHOICE_AUTO;
      else if (strcmp(v, "none") == 0)
         out->tool_choice = AIMEE_TOOL_CHOICE_NONE;
      else if (strcmp(v, "required") == 0)
         out->tool_choice = AIMEE_TOOL_CHOICE_ANY;
      else
         goto unsupported;
      return 0;
   }
   if (cJSON_IsObject(tc))
   {
      const char *type = ostr(tc, "type");
      const char *name = NULL;
      if (type && strcmp(type, "function") == 0)
      {
         if (responses_shape)
            name = ostr(tc, "name");
         else
            name = ostr(cJSON_GetObjectItemCaseSensitive((cJSON *)tc, "function"), "name");
      }
      if (!name || !name[0])
         goto unsupported;
      out->tool_choice = AIMEE_TOOL_CHOICE_TOOL;
      out->tool_choice_name = dupstr(name);
      return out->tool_choice_name ? 0 : -2;
   }
unsupported:
   /* A hosted-tool, allowed_tools or custom choice names something the neutral IR
    * has no field for. Refuse it rather than downgrade it: forcing a different tool
    * set than the client asked for is a semantic change, not a translation. */
   if (err && errn)
      snprintf(err, errn, "%s_frontend_parse: unsupported tool_choice",
               responses_shape ? "responses" : "openai");
   return -1;
}

/* A hosted tool is owned by its provider. Configuration is opaque data attached
 * to that explicit typed capability; it is never mistaken for a local function. */
static int openai_hosted_tool_type(const char *type)
{
   static const char *const types[] = {"web_search",
                                       "web_search_preview",
                                       "web_search_preview_2025_03_11",
                                       "file_search",
                                       "computer",
                                       "computer_use_preview",
                                       "code_interpreter",
                                       "image_generation",
                                       "local_shell",
                                       "shell",
                                       "apply_patch",
                                       "mcp",
                                       "tool_search"};
   for (size_t i = 0; type && i < sizeof types / sizeof types[0]; i++)
      if (!strcmp(type, types[i]))
         return 1;
   return 0;
}
int openai_ir_parse_tool(const cJSON *native, aimee_tool_t *tool, int nested)
{
   if (!cJSON_IsObject(native) || !tool)
      return -1;
   const char *type = ostr(native, "type");
   const cJSON *definition = cJSON_GetObjectItemCaseSensitive(native, "function");
   if (!definition)
      definition = native;
   if (type && !strcmp(type, "namespace"))
   {
      if (nested)
         return -1;
      tool->kind = AIMEE_TOOL_NAMESPACE;
      definition = native;
      const cJSON *children = cJSON_GetObjectItemCaseSensitive(native, "tools");
      int count = cJSON_GetArraySize(children);
      if (!cJSON_IsArray(children) || count > AIMEE_IR_MAX_BLOCKS)
         return -1;
      tool->children = count ? calloc((size_t)count, sizeof *tool->children) : NULL;
      if (count && !tool->children)
         return -1;
      tool->n_children = count;
      for (int i = 0; i < count; i++)
         if (openai_ir_parse_tool(cJSON_GetArrayItem(children, i), &tool->children[i], 1) ||
             tool->children[i].kind != AIMEE_TOOL_FUNCTION)
            return -1;
   }
   else if (type && strcmp(type, "function"))
   {
      if (nested || !openai_hosted_tool_type(type))
         return -1;
      tool->kind = AIMEE_TOOL_HOSTED;
      tool->provider_wire = AIMEE_WIRE_RESPONSES;
      tool->native_type = strdup(type);
      definition = native;
      if (!tool->native_type)
         return -1;
   }
   tool->name = dupstr(ostr(definition, "name"));
   tool->description = dupstr(ostr(definition, "description"));
   const cJSON *schema = cJSON_GetObjectItemCaseSensitive(definition, "parameters");
   const cJSON *strict = cJSON_GetObjectItemCaseSensitive(definition, "strict");
   const cJSON *defer = cJSON_GetObjectItemCaseSensitive(definition, "defer_loading");
   if ((schema && !cJSON_IsObject(schema)) ||
       (strict && !cJSON_IsNull(strict) && !cJSON_IsBool(strict)) ||
       (defer && !cJSON_IsBool(defer)))
      return -1;
   tool->schema = schema ? cJSON_Duplicate(schema, 1) : NULL;
   tool->strict = !strict                ? AIMEE_STRICT_UNSET
                  : cJSON_IsNull(strict) ? AIMEE_STRICT_NULL
                  : cJSON_IsTrue(strict) ? AIMEE_STRICT_TRUE
                                         : AIMEE_STRICT_FALSE;
   tool->has_defer_loading = defer != NULL;
   tool->defer_loading = cJSON_IsTrue(defer);
   tool->raw = cJSON_Duplicate(native, 1);
   tool->native_options = cJSON_Duplicate(definition, 1);
   if (!tool->raw || !tool->native_options || (schema && !tool->schema))
      return -1;
   const char *fields[] = {"type",   "name",          "description", "parameters",
                           "strict", "defer_loading", "tools"};
   for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++)
      cJSON_DeleteItemFromObjectCaseSensitive(tool->native_options, fields[i]);
   if (!cJSON_GetArraySize(tool->native_options))
   {
      cJSON_Delete(tool->native_options);
      tool->native_options = NULL;
   }
   if (tool->native_options)
      tool->provider_wire = AIMEE_WIRE_RESPONSES;
   return aimee_ir_tool_valid(tool) ? 0 : -1;
}
cJSON *openai_ir_render_tool(const aimee_tool_t *tool, int responses_shape)
{
   if (!aimee_ir_tool_valid(tool) ||
       (tool->kind == AIMEE_TOOL_HOSTED && tool->provider_wire != AIMEE_WIRE_RESPONSES) ||
       (!responses_shape && tool->kind != AIMEE_TOOL_FUNCTION))
      return NULL;
   cJSON *out = cJSON_CreateObject();
   const char *type = tool->kind == AIMEE_TOOL_NAMESPACE ? "namespace"
                      : tool->kind == AIMEE_TOOL_HOSTED  ? tool->native_type
                                                         : "function";
   if (!out || !cJSON_AddStringToObject(out, "type", type))
      goto fail;
   cJSON *definition = !responses_shape ? cJSON_AddObjectToObject(out, "function") : out;
   if (!definition)
      goto fail;
   if (tool->native_options)
   {
      const cJSON *option;
      cJSON_ArrayForEach(option, tool->native_options)
      {
         if (!option->string || cJSON_GetObjectItemCaseSensitive(definition, option->string) ||
             !strcmp(option->string, "name") || !strcmp(option->string, "description") ||
             !strcmp(option->string, "parameters") || !strcmp(option->string, "strict") ||
             !strcmp(option->string, "defer_loading") || !strcmp(option->string, "tools"))
            goto fail;
         if (!cJSON_AddItemToObject(definition, option->string, cJSON_Duplicate(option, 1)))
            goto fail;
      }
   }
   if ((tool->name && !cJSON_AddStringToObject(definition, "name", tool->name)) ||
       (tool->description &&
        !cJSON_AddStringToObject(definition, "description", tool->description)))
      goto fail;
   if (tool->kind == AIMEE_TOOL_FUNCTION)
   {
      if (!cJSON_AddItemToObject(definition, "parameters",
                                 tool->schema ? cJSON_Duplicate(tool->schema, 1)
                                              : cJSON_CreateObject()))
         goto fail;
      if (tool->strict == AIMEE_STRICT_NULL)
      {
         if (!cJSON_AddNullToObject(definition, "strict"))
            goto fail;
      }
      else if (tool->strict != AIMEE_STRICT_UNSET &&
               !cJSON_AddBoolToObject(definition, "strict", tool->strict == AIMEE_STRICT_TRUE))
         goto fail;
      if (tool->has_defer_loading &&
          !cJSON_AddBoolToObject(definition, "defer_loading", tool->defer_loading))
         goto fail;
   }
   else if (tool->kind == AIMEE_TOOL_NAMESPACE)
   {
      cJSON *children = cJSON_AddArrayToObject(out, "tools");
      if (!children)
         goto fail;
      for (int i = 0; i < tool->n_children; i++)
      {
         cJSON *child = openai_ir_render_tool(&tool->children[i], 1);
         if (!child || !cJSON_AddItemToArray(children, child))
         {
            cJSON_Delete(child);
            goto fail;
         }
      }
   }
   return out;
fail:
   cJSON_Delete(out);
   return NULL;
}

int openai_frontend_parse(const cJSON *req, aimee_request_t *out, char *err, size_t errn)
{
   if (out)
      memset(out, 0, sizeof *out);
   if (!req || !cJSON_IsObject(req) || !out)
   {
      if (err && errn)
         snprintf(err, errn, "openai_frontend_parse: null/non-object request");
      return -1;
   }
   out->frontend = AIMEE_WIRE_OPENAI_CHAT;
   out->raw = cJSON_Duplicate(req, 1);
   out->model = dupstr(ostr(req, "model"));

   const cJSON *mt = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "max_tokens");
   if (mt && cJSON_IsNumber(mt))
   {
      out->max_tokens = mt->valueint;
      out->has_max_tokens = 1;
   }
   const cJSON *temp = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "temperature");
   if (temp && cJSON_IsNumber(temp))
   {
      out->temperature = temp->valuedouble;
      out->has_temperature = 1;
   }
   const cJSON *top_p = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "top_p");
   if (top_p && cJSON_IsNumber(top_p))
   {
      out->top_p = top_p->valuedouble;
      out->has_top_p = 1;
   }
   const cJSON *top_k = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "top_k");
   if (top_k && cJSON_IsNumber(top_k))
   {
      out->top_k = top_k->valueint;
      out->has_top_k = 1;
   }
   if (openai_family_prompt_cache_parse(req, out, err, errn))
   {
      aimee_request_free(out);
      return -1;
   }
   const cJSON *meta = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "metadata");
   if (meta && !cJSON_IsNull(meta))
      out->metadata = cJSON_Duplicate(meta, 1);
   const cJSON *stream = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "stream");
   out->stream = (stream && cJSON_IsTrue(stream)) ? 1 : 0;

   if (openai_family_output_format_parse(cJSON_GetObjectItemCaseSensitive(req, "response_format"),
                                         0, out, err, errn))
   {
      aimee_request_free(out);
      return -1;
   }

   /* messages. LIFT only the LEADING CONSECUTIVE run of role:"system" into
    * out->system (to converge with Anthropic's separate system). A system message
    * AFTER the first non-system turn is NOT lifted -- lifting mid-conversation
    * system would elevate later user-controlled content to system trust (a
    * privilege-escalation / injection vector, ruling Q2) -- it is kept as a normal
    * message with role preserved. Empty/whitespace-only system is dropped. */
   const cJSON *msgs = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "messages");
   if (msgs && cJSON_IsArray(msgs))
   {
      int leading = 1;
      int tool_run = -1; /* index into out->messages of the pending tool-result user msg */
      const cJSON *m = NULL;
      cJSON_ArrayForEach(m, msgs)
      {
         const char *role = ostr(m, "role");
         const cJSON *content = cJSON_GetObjectItemCaseSensitive((cJSON *)m, "content");
         if (leading && role && strcmp(role, "system") == 0)
         {
            const char *s = cJSON_IsString(content) ? content->valuestring : NULL;
            int empty = s && s[strspn(s, " \t\r\n")] == '\0'; /* whitespace-only */
            if (!empty && openai_ir_parse_content(content, &out->system, &out->n_system) != 0)
               goto oom;
            continue;
         }
         leading = 0; /* the leading system run has ended */
         /* MERGE (grouping ruling, Option A): a maximal run of consecutive
          * role:"tool" messages becomes tool_result BLOCKS in ONE canonical user
          * message. call_id -> tool_id verbatim; content string -> tool_result. */
         if (role && strcmp(role, "tool") == 0)
         {
            if (tool_run < 0)
            {
               aimee_message_t *tr =
                   grow1((void **)&out->messages, &out->n_messages, sizeof(aimee_message_t));
               if (!tr)
                  goto oom;
               tr->role = dupstr("user");
               tool_run = out->n_messages - 1;
            }
            aimee_message_t *tr = &out->messages[tool_run];
            aimee_block_t *b = grow1((void **)&tr->blocks, &tr->n_blocks, sizeof(aimee_block_t));
            if (!b)
               goto oom;
            b->type = AIMEE_BLK_TOOL_RESULT;
            b->raw = cJSON_Duplicate(m, 1);
            b->tool_id = dupstr(ostr(m, "tool_call_id"));
            const char *tc = cJSON_IsString(content) ? content->valuestring : NULL;
            b->tool_result =
                tc ? cJSON_CreateString(tc) : (content ? cJSON_Duplicate(content, 1) : NULL);
            continue;
         }
         tool_run = -1; /* any non-tool message ends the run */
         aimee_message_t *msg =
             grow1((void **)&out->messages, &out->n_messages, sizeof(aimee_message_t));
         if (!msg)
            goto oom;
         msg->role = dupstr(role);
         msg->raw = cJSON_Duplicate(m, 1);
         /* Some Chat-compatible providers return assistant reasoning separately
          * from content. Preserve it at entry, before visible content and calls. */
         const char *reasoning =
             role && !strcmp(role, "assistant") ? ostr(m, "reasoning_content") : NULL;
         if (!reasoning && role && !strcmp(role, "assistant"))
            reasoning = ostr(m, "reasoning");
         if (reasoning && reasoning[0])
         {
            aimee_block_t *block = grow1((void **)&msg->blocks, &msg->n_blocks, sizeof(*block));
            if (!block)
               goto oom;
            block->type = AIMEE_BLK_THINKING;
            block->text = dupstr(reasoning);
            if (!block->text)
               goto oom;
         }
         if (openai_ir_parse_content(content, &msg->blocks, &msg->n_blocks) != 0)
            goto oom;
         /* assistant tool_calls -> TOOL_USE blocks. tool_input is the PARSED object
          * (canonical, cross-protocol comparable); the OpenAI `arguments` STRING is
          * preserved in the block raw sidecar for same-protocol replay (ruling Q1
          * option C: opaque bytes scoped to same-protocol). tool_id stays a separate
          * typed field, carried verbatim (never derived from the args). */
         const cJSON *calls = cJSON_GetObjectItemCaseSensitive((cJSON *)m, "tool_calls");
         if (calls && cJSON_IsArray(calls))
         {
            const cJSON *call = NULL;
            cJSON_ArrayForEach(call, calls)
            {
               aimee_block_t *b =
                   grow1((void **)&msg->blocks, &msg->n_blocks, sizeof(aimee_block_t));
               if (!b)
                  goto oom;
               b->type = AIMEE_BLK_TOOL_USE;
               b->raw = cJSON_Duplicate(call, 1);
               b->tool_id = dupstr(ostr(call, "id"));
               b->tool_namespace = dupstr(ostr(call, "namespace"));
               const cJSON *fn = cJSON_GetObjectItemCaseSensitive((cJSON *)call, "function");
               b->tool_name = dupstr(fn ? ostr(fn, "name") : NULL);
               const char *args = fn ? ostr(fn, "arguments") : NULL;
               b->tool_input = args ? aimee_ir_json_parse_frame(args, strlen(args))
                                    : NULL; /* derived-once parse */
               if (!cJSON_IsObject(b->tool_input))
               {
                  if (err && errn)
                     snprintf(err, errn,
                              "completed function arguments require a complete JSON object");
                  aimee_request_free(out);
                  return -1;
               }
            }
         }
      }
   }

   const cJSON *effort = cJSON_GetObjectItemCaseSensitive(req, "reasoning_effort");
   if (effort && !cJSON_IsNull(effort) &&
       (!cJSON_IsString(effort) ||
        aimee_ir_effort_parse(effort->valuestring, &out->reasoning_effort)))
   {
      aimee_request_free(out);
      return -1;
   }

   /* tools: {type:function, function:{name,description,parameters}} */
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

   switch (openai_family_tool_choice_parse(
       cJSON_GetObjectItemCaseSensitive((cJSON *)req, "tool_choice"),
       cJSON_GetObjectItemCaseSensitive((cJSON *)req, "parallel_tool_calls"), 0, out, err, errn))
   {
   case 0:
      break;
   case -1:
      aimee_request_free(out);
      return -1;
   default:
      goto oom;
   }

   /* stop: string OR array */
   const cJSON *stop = cJSON_GetObjectItemCaseSensitive((cJSON *)req, "stop");
   if (stop && cJSON_IsString(stop))
   {
      out->stop_sequences = calloc(1, sizeof(char *));
      if (!out->stop_sequences)
         goto oom;
      out->stop_sequences[0] = dupstr(stop->valuestring);
      out->n_stop = 1;
   }
   else if (stop && cJSON_IsArray(stop))
   {
      const cJSON *s = NULL;
      cJSON_ArrayForEach(s, stop)
      {
         if (!cJSON_IsString(s))
            continue;
         char **slot = grow1((void **)&out->stop_sequences, &out->n_stop, sizeof(char *));
         if (!slot)
            goto oom;
         *slot = dupstr(s->valuestring);
      }
   }
   return 0;

oom:
   if (err && errn)
      snprintf(err, errn, "openai_frontend_parse: out of memory");
   aimee_request_free(out);
   return -1;
}

/* The legacy /v1/completions surface owns only a text prompt. Build its IR
 * directly at entry, without fabricating an intermediate provider protocol. */
int aimee_frontend_parse_text_completion(const char *body, size_t length, aimee_request_t *out,
                                         char *error, size_t capacity)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   cJSON *root =
       body && length && length != SIZE_MAX ? aimee_ir_json_parse_frame(body, length) : NULL;
   const char *prompt = ostr(root, "prompt");
   const cJSON *model = cJSON_GetObjectItemCaseSensitive(root, "model");
   const cJSON *stream = cJSON_GetObjectItemCaseSensitive(root, "stream");
   const cJSON *maximum = cJSON_GetObjectItemCaseSensitive(root, "max_tokens");
   const cJSON *temperature = cJSON_GetObjectItemCaseSensitive(root, "temperature");
   const cJSON *top_p = cJSON_GetObjectItemCaseSensitive(root, "top_p");
   const cJSON *top_k = cJSON_GetObjectItemCaseSensitive(root, "top_k");
   const cJSON *stop = cJSON_GetObjectItemCaseSensitive(root, "stop");
   if (cJSON_IsNull(model))
      model = NULL;
   if (cJSON_IsNull(stream))
      stream = NULL;
   if (cJSON_IsNull(maximum))
      maximum = NULL;
   if (cJSON_IsNull(temperature))
      temperature = NULL;
   if (cJSON_IsNull(top_p))
      top_p = NULL;
   if (cJSON_IsNull(top_k))
      top_k = NULL;
   if (!cJSON_IsObject(root) || !prompt || !prompt[0] || (model && !cJSON_IsString(model)) ||
       (stream && !cJSON_IsBool(stream)) ||
       (maximum &&
        (!cJSON_IsNumber(maximum) || !isfinite(maximum->valuedouble) || maximum->valuedouble < 1 ||
         maximum->valuedouble > INT_MAX || maximum->valuedouble != floor(maximum->valuedouble))) ||
       (temperature && (!cJSON_IsNumber(temperature) || !isfinite(temperature->valuedouble) ||
                        temperature->valuedouble < 0 || temperature->valuedouble > 2)) ||
       (top_p && (!cJSON_IsNumber(top_p) || !isfinite(top_p->valuedouble) ||
                  top_p->valuedouble < 0 || top_p->valuedouble > 1)) ||
       (top_k &&
        (!cJSON_IsNumber(top_k) || !isfinite(top_k->valuedouble) || top_k->valuedouble < 1 ||
         top_k->valuedouble > INT_MAX || top_k->valuedouble != floor(top_k->valuedouble))) ||
       (stop && !cJSON_IsNull(stop) && !cJSON_IsString(stop) && !cJSON_IsArray(stop)) ||
       cJSON_GetObjectItemCaseSensitive(root, "tools"))
      goto invalid;
   const char *single[] = {"n", "best_of", NULL};
   for (int i = 0; single[i]; i++)
   {
      const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, single[i]);
      if (value && !cJSON_IsNull(value) && (!cJSON_IsNumber(value) || value->valuedouble != 1))
         goto invalid;
   }
   const cJSON *echo = cJSON_GetObjectItemCaseSensitive(root, "echo");
   if (echo && !cJSON_IsNull(echo) && !cJSON_IsFalse(echo))
      goto invalid;
   out->model = model ? strdup(model->valuestring) : NULL;
   out->stream = cJSON_IsTrue(stream);
   out->has_max_tokens = maximum != NULL;
   out->max_tokens = maximum ? maximum->valueint : 0;
   out->has_temperature = temperature != NULL;
   out->temperature = temperature ? temperature->valuedouble : 0;
   out->has_top_p = top_p != NULL;
   out->top_p = top_p ? top_p->valuedouble : 0;
   out->has_top_k = top_k != NULL;
   out->top_k = top_k ? top_k->valueint : 0;
   const cJSON *metadata = cJSON_GetObjectItemCaseSensitive(root, "metadata");
   if (metadata && !cJSON_IsNull(metadata))
   {
      if (!cJSON_IsObject(metadata) || !(out->metadata = cJSON_Duplicate(metadata, 1)))
         goto invalid;
   }
   out->messages = calloc(1, sizeof *out->messages);
   if ((model && !out->model) || !out->messages)
      goto invalid;
   out->n_messages = 1;
   out->messages[0].role = strdup("user");
   out->messages[0].blocks = calloc(1, sizeof *out->messages[0].blocks);
   if (!out->messages[0].role || !out->messages[0].blocks)
      goto invalid;
   out->messages[0].n_blocks = 1;
   out->messages[0].blocks[0] = (aimee_block_t){.type = AIMEE_BLK_TEXT, .text = strdup(prompt)};
   if (!out->messages[0].blocks[0].text)
      goto invalid;
   out->n_stop = cJSON_IsString(stop) ? 1 : cJSON_IsArray(stop) ? cJSON_GetArraySize(stop) : 0;
   if (out->n_stop > AIMEE_IR_MAX_BLOCKS)
      goto invalid;
   if (out->n_stop)
   {
      out->stop_sequences = calloc((size_t)out->n_stop, sizeof *out->stop_sequences);
      if (!out->stop_sequences)
         goto invalid;
      for (int i = 0; i < out->n_stop; i++)
      {
         const cJSON *value = cJSON_IsString(stop) ? stop : cJSON_GetArrayItem(stop, i);
         if (!cJSON_IsString(value) || !(out->stop_sequences[i] = strdup(value->valuestring)))
            goto invalid;
      }
   }
   /* Owned input sidecar for explicit external replay/representation checks.
    * This legacy wire has no native-byte fast path and is never Chat history. */
   out->raw = cJSON_Duplicate(root, 1);
   if (!out->raw)
      goto invalid;
   cJSON_Delete(root);
   return 0;
invalid:
   cJSON_Delete(root);
   aimee_request_free(out);
   if (error && capacity)
      snprintf(error, capacity, "invalid or unsupported text completion request");
   return -1;
}

/* canonical stop_reason -> OpenAI finish_reason */
static const char *openai_finish(const aimee_response_t *r)
{
   switch (r->stop_reason)
   {
   case AIMEE_STOP_TOOL_USE:
      return "tool_calls";
   case AIMEE_STOP_MAX_TOKENS:
      return "length";
   case AIMEE_STOP_CONTENT_FILTER:
      return "content_filter";
   case AIMEE_STOP_END_TURN:
   case AIMEE_STOP_STOP_SEQUENCE:
      return "stop";
   default:
      return "stop";
   }
}

/* Content arrays retain the ordering of text and media. Unrepresentable files
 * fail at this egress boundary rather than silently disappearing from history. */
cJSON *openai_ir_render_content(const aimee_message_t *message)
{
   cJSON *parts = cJSON_CreateArray();
   if (!parts)
      return NULL;
   for (int i = 0; i < message->n_blocks; i++)
   {
      const aimee_block_t *block = &message->blocks[i];
      if (block->type != AIMEE_BLK_TEXT && block->type != AIMEE_BLK_IMAGE &&
          block->type != AIMEE_BLK_DOCUMENT)
         continue;
      cJSON *part = cJSON_CreateObject();
      if (!part)
         goto fail;
      if (block->type == AIMEE_BLK_TEXT)
      {
         cJSON_AddStringToObject(part, "type", "text");
         cJSON_AddStringToObject(part, "text", block->text ? block->text : "");
      }
      else
      {
         char *ref = aimee_ir_media_url(block);
         if (!ref)
         {
            cJSON_Delete(part);
            goto fail;
         }
         if (block->type == AIMEE_BLK_IMAGE)
         {
            cJSON_AddStringToObject(part, "type", "image_url");
            cJSON *image = cJSON_AddObjectToObject(part, "image_url");
            cJSON_AddStringToObject(image, "url", ref);
         }
         else
         {
            if (strncmp(ref, "file-", 5) && strncmp(ref, "data:", 5))
            {
               free(ref);
               cJSON_Delete(part);
               goto fail;
            }
            cJSON_AddStringToObject(part, "type", "file");
            cJSON *file = cJSON_AddObjectToObject(part, "file");
            cJSON_AddStringToObject(file, !strncmp(ref, "file-", 5) ? "file_id" : "file_data", ref);
         }
         free(ref);
      }
      cJSON_AddItemToArray(parts, part);
   }
   return parts;
fail:
   cJSON_Delete(parts);
   return NULL;
}

/* This wire has no representation for typed provider refusal diagnostics. */
cJSON *openai_frontend_render(const aimee_response_t *r)
{
   if (!r || r->refusal)
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
   cJSON_AddStringToObject(out, "object", "chat.completion");
   if (r->model)
      cJSON_AddStringToObject(out, "model", r->model);

   cJSON *choices = cJSON_AddArrayToObject(out, "choices");
   cJSON *choice = cJSON_CreateObject();
   cJSON_AddItemToArray(choices, choice);
   cJSON_AddNumberToObject(choice, "index", 0);
   cJSON *msg = cJSON_AddObjectToObject(choice, "message");
   cJSON_AddStringToObject(msg, "role", r->role ? r->role : "assistant");

   /* Presentation follows authored-message grouping; replay retains original blocks. */
   char *text = aimee_ir_response_text_alloc(r);
   if (!text)
   {
      cJSON_Delete(out);
      return NULL;
   }
   cJSON *tool_calls = NULL;
   if (text)
   {
      for (int i = 0; i < r->n_content; i++)
      {
         const aimee_block_t *b = &r->content[i];
         if (b->type == AIMEE_BLK_TOOL_USE)
         {
            if (!tool_calls)
               tool_calls = cJSON_CreateArray();
            cJSON *call = cJSON_CreateObject();
            cJSON_AddStringToObject(call, "id", b->tool_id ? b->tool_id : "");
            cJSON_AddStringToObject(call, "type", "function");
            if (b->tool_namespace && b->tool_namespace[0])
               cJSON_AddStringToObject(call, "namespace", b->tool_namespace);
            cJSON *fn = cJSON_AddObjectToObject(call, "function");
            cJSON_AddStringToObject(fn, "name", b->tool_name ? b->tool_name : "");
            /* arguments STRING: serialize the parsed tool_input (cross-protocol) */
            char *args = b->tool_input ? cJSON_PrintUnformatted(b->tool_input) : NULL;
            cJSON_AddStringToObject(fn, "arguments", args ? args : "{}");
            free(args);
            cJSON_AddItemToArray(tool_calls, call);
         }
      }
   }
   int has_media = 0;
   for (int i = 0; i < r->n_content; i++)
      has_media |=
          r->content[i].type == AIMEE_BLK_IMAGE || r->content[i].type == AIMEE_BLK_DOCUMENT;
   if (has_media)
   {
      aimee_message_t message = {
          .role = "assistant", .blocks = r->content, .n_blocks = r->n_content};
      cJSON *content = openai_ir_render_content(&message);
      if (!content)
      {
         free(text);
         cJSON_Delete(tool_calls);
         cJSON_Delete(out);
         return NULL;
      }
      cJSON_AddItemToObject(msg, "content", content);
   }
   else if (text[0])
      cJSON_AddStringToObject(msg, "content", text);
   else
      cJSON_AddNullToObject(msg, "content");
   free(text);
   for (int i = 0; i < r->n_content; i++)
      if (r->content[i].type == AIMEE_BLK_THINKING && r->content[i].text)
      {
         const cJSON *old = cJSON_GetObjectItemCaseSensitive(msg, "reasoning_content");
         const char *previous = cJSON_GetStringValue(old);
         const char *next = r->content[i].text;
         size_t length = (previous ? strlen(previous) : 0) + strlen(next) + 1;
         char *joined = malloc(length);
         if (!joined)
         {
            cJSON_Delete(tool_calls);
            cJSON_Delete(out);
            return NULL;
         }
         snprintf(joined, length, "%s%s", previous ? previous : "", next);
         cJSON_DeleteItemFromObjectCaseSensitive(msg, "reasoning_content");
         cJSON_AddStringToObject(msg, "reasoning_content", joined);
         free(joined);
      }
   if (tool_calls)
      cJSON_AddItemToObject(msg, "tool_calls", tool_calls);

   cJSON_AddStringToObject(choice, "finish_reason", openai_finish(r));

   if (r->usage_unobserved)
      return out; /* Optional usage is absent; no measured zero was observed. */
   cJSON *usage = cJSON_AddObjectToObject(out, "usage");
   cJSON_AddNumberToObject(usage, "prompt_tokens", (double)r->usage_in);
   cJSON_AddNumberToObject(usage, "completion_tokens", (double)r->usage_out);
   cJSON_AddNumberToObject(usage, "total_tokens", (double)r->usage_in + (double)r->usage_out);
   cJSON *cached = cJSON_AddObjectToObject(usage, "prompt_tokens_details");
   cJSON_AddNumberToObject(cached, "cached_tokens", (double)r->usage_cache_read);
   cJSON *reasoning = cJSON_AddObjectToObject(usage, "completion_tokens_details");
   cJSON_AddNumberToObject(reasoning, "reasoning_tokens", (double)r->usage_reasoning);
   return out;
}

/* The provider result remains owned IR through policy and the client boundary. */
cJSON *aimee_openai_render_response(const aimee_response_t *response, const char *id,
                                    const char *model, long created, int responses)
{
   if (!response || !id || !model)
      return NULL;
   aimee_response_t view = *response;
   view.id = (char *)id;
   view.model = (char *)model;
   cJSON *root = responses ? responses_frontend_render(&view) : openai_frontend_render(&view);
   if (!root)
      return NULL;
   cJSON_AddNumberToObject(root, responses ? "created_at" : "created", (double)created);
   if (responses)
   {
      cJSON *item;
      int index = 0;
      cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "output"))
      {
         if (!cJSON_GetObjectItemCaseSensitive(item, "id"))
         {
            size_t capacity = strlen(id) + 40;
            char *item_id = malloc(capacity);
            if (!item_id)
            {
               cJSON_Delete(root);
               return NULL;
            }
            snprintf(item_id, capacity, "%s-item-%d", id, index);
            cJSON_AddStringToObject(item, "id", item_id);
            free(item_id);
         }
         cJSON_AddStringToObject(item, "status", "completed");
         index++;
      }
   }
   return root;
}
int aimee_openai_buffered_response(const aimee_response_t *response, const char *id,
                                   const char *model, long created, int responses, char *buffer,
                                   int capacity)
{
   if (!buffer || capacity <= 0)
      return -1;
   cJSON *root = aimee_openai_render_response(response, id, model, created, responses);
   int length =
       root && cJSON_PrintPreallocated(root, buffer, capacity, 0) ? (int)strlen(buffer) : -1;
   cJSON_Delete(root);
   return length;
}
static int openai_ir_emit_event(aimee_sse_emit_fn emit, void *context, const char *event,
                                cJSON *payload)
{
   if (!payload || !cJSON_AddStringToObject(payload, "type", event))
   {
      cJSON_Delete(payload);
      return -1;
   }
   char *json = cJSON_PrintUnformatted(payload);
   cJSON_Delete(payload);
   if (!json)
      return -1;
   emit(context, event, json);
   free(json);
   return 0;
}
int aimee_openai_emit_chat_response(const aimee_response_t *response, const char *id,
                                    const char *model, long created, aimee_chat_emit_fn emit,
                                    void *context)
{
   if (!emit)
      return -1;
   cJSON *root = aimee_openai_render_response(response, id, model, created, 0);
   if (!root)
      return -1;
   cJSON *choice = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "choices"), 0);
   cJSON *message = cJSON_DetachItemFromObjectCaseSensitive(choice, "message");
   cJSON *calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls"), *call;
   int index = 0;
   cJSON_ArrayForEach(call, calls) cJSON_AddNumberToObject(call, "index", index++);
   cJSON_AddItemToObject(choice, "delta", message);
   cJSON *finish = cJSON_DetachItemFromObjectCaseSensitive(choice, "finish_reason");
   cJSON_AddNullToObject(choice, "finish_reason");
   cJSON_ReplaceItemInObjectCaseSensitive(root, "object",
                                          cJSON_CreateString("chat.completion.chunk"));
   cJSON *usage = cJSON_DetachItemFromObjectCaseSensitive(root, "usage");
   char *json = cJSON_PrintUnformatted(root);
   if (!json)
   {
      cJSON_Delete(finish);
      cJSON_Delete(usage);
      cJSON_Delete(root);
      return -1;
   }
   emit(context, json);
   free(json);
   cJSON_ReplaceItemInObjectCaseSensitive(choice, "delta", cJSON_CreateObject());
   cJSON_ReplaceItemInObjectCaseSensitive(choice, "finish_reason", finish);
   cJSON_AddItemToObject(root, "usage", usage);
   json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!json)
      return -1;
   emit(context, json);
   free(json);
   return 0;
}
static cJSON *openai_ir_item_event(const cJSON *item, int index)
{
   cJSON *event = cJSON_CreateObject();
   if (!event || !cJSON_AddNumberToObject(event, "output_index", index) ||
       !cJSON_AddStringToObject(event, "item_id",
                                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "id"))))
   {
      cJSON_Delete(event);
      return NULL;
   }
   return event;
}
int aimee_openai_emit_responses_tail(const aimee_response_t *response, const char *id,
                                     const char *model, long created, aimee_sse_emit_fn emit,
                                     void *context)
{
   if (!emit)
      return -1;
   cJSON *root = aimee_openai_render_response(response, id, model, created, 1);
   if (!root)
      return -1;
   cJSON *item;
   int index = 0, rc = -1;
   cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "output"))
   {
      const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "type"));
      cJSON *start = cJSON_Duplicate(item, 1);
      cJSON *event = openai_ir_item_event(item, index);
      if (!start || !event)
      {
         cJSON_Delete(start);
         cJSON_Delete(event);
         goto done;
      }
      cJSON_ReplaceItemInObjectCaseSensitive(start, "status", cJSON_CreateString("in_progress"));
      if (!strcmp(type, "message"))
         cJSON_ReplaceItemInObjectCaseSensitive(start, "content", cJSON_CreateArray());
      else if (!strcmp(type, "function_call"))
         cJSON_ReplaceItemInObjectCaseSensitive(start, "arguments", cJSON_CreateString(""));
      else if (!strcmp(type, "reasoning"))
      {
         cJSON_ReplaceItemInObjectCaseSensitive(start, "summary", cJSON_CreateArray());
         cJSON_DeleteItemFromObjectCaseSensitive(start, "encrypted_content");
      }
      cJSON_AddItemToObject(event, "item", start);
      if (openai_ir_emit_event(emit, context, "response.output_item.added", event))
         goto done;
      if (!strcmp(type, "function_call"))
      {
         const char *args =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "arguments"));
         event = openai_ir_item_event(item, index);
         cJSON_AddStringToObject(event, "delta", args);
         if (openai_ir_emit_event(emit, context, "response.function_call_arguments.delta", event))
            goto done;
         event = openai_ir_item_event(item, index);
         cJSON_AddStringToObject(event, "arguments", args);
         if (openai_ir_emit_event(emit, context, "response.function_call_arguments.done", event))
            goto done;
      }
      else
      {
         int reasoning = !strcmp(type, "reasoning");
         const char *parts_name = reasoning ? "summary" : "content";
         cJSON *part;
         int part_index = 0;
         cJSON_ArrayForEach(part, cJSON_GetObjectItemCaseSensitive(item, parts_name))
         {
            const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(part, "text"));
            const char *position = reasoning ? "summary_index" : "content_index";
            const char *added =
                reasoning ? "response.reasoning_summary_part.added" : "response.content_part.added";
            const char *delta =
                reasoning ? "response.reasoning_summary_text.delta" : "response.output_text.delta";
            const char *text_done =
                reasoning ? "response.reasoning_summary_text.done" : "response.output_text.done";
            const char *part_done =
                reasoning ? "response.reasoning_summary_part.done" : "response.content_part.done";
            cJSON *empty = cJSON_Duplicate(part, 1);
            cJSON_ReplaceItemInObjectCaseSensitive(empty, "text", cJSON_CreateString(""));
            event = openai_ir_item_event(item, index);
            cJSON_AddNumberToObject(event, position, part_index);
            cJSON_AddItemToObject(event, "part", empty);
            if (openai_ir_emit_event(emit, context, added, event))
               goto done;
            event = openai_ir_item_event(item, index);
            cJSON_AddNumberToObject(event, position, part_index);
            cJSON_AddStringToObject(event, "delta", text ? text : "");
            if (openai_ir_emit_event(emit, context, delta, event))
               goto done;
            event = openai_ir_item_event(item, index);
            cJSON_AddNumberToObject(event, position, part_index);
            cJSON_AddStringToObject(event, "text", text ? text : "");
            if (openai_ir_emit_event(emit, context, text_done, event))
               goto done;
            event = openai_ir_item_event(item, index);
            cJSON_AddNumberToObject(event, position, part_index);
            cJSON_AddItemToObject(event, "part", cJSON_Duplicate(part, 1));
            if (openai_ir_emit_event(emit, context, part_done, event))
               goto done;
            part_index++;
         }
      }
      event = openai_ir_item_event(item, index);
      cJSON_AddItemToObject(event, "item", cJSON_Duplicate(item, 1));
      if (openai_ir_emit_event(emit, context, "response.output_item.done", event))
         goto done;
      index++;
   }
   cJSON *terminal = cJSON_CreateObject();
   cJSON_AddItemToObject(terminal, "response", cJSON_Duplicate(root, 1));
   rc = openai_ir_emit_event(emit, context,
                             response->stop_reason == AIMEE_STOP_MAX_TOKENS ? "response.incomplete"
                                                                            : "response.completed",
                             terminal);
done:
   cJSON_Delete(root);
   return rc;
}
