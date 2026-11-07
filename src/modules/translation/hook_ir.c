/* Native hook envelope admission. No environment, storage, routing or policy. */
#include <aimee/translation/hook_ir.h>
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

static cJSON *parse_frame(const char *frame, size_t length)
{
   return length > AIMEE_HOOK_FRAME_MAX ? NULL : aimee_ir_json_parse_frame(frame, length);
}
static int text(const cJSON *object, const char *key, char **out, int required)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
   if (!value)
      return required ? -1 : 0;
   if (!cJSON_IsString(value) || (required && !value->valuestring[0]))
      return -1;
   *out = strdup(value->valuestring);
   return *out ? 0 : -1;
}
static int cwd(const cJSON *object, char **out)
{
   static const char *const keys[] = {"cwd", "workdir", "working_dir", "working_directory"};
   for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
   {
      char *value = NULL;
      if (text(object, keys[i], &value, 0))
         return -1;
      if (value && strlen(value) >= 4096)
      {
         free(value);
         return -1;
      }
      if (value && value[0])
      {
         if (*out && strcmp(*out, value))
         {
            free(value);
            return -1;
         }
         if (!*out)
            *out = value;
         else
            free(value);
      }
      else
         free(value);
   }
   return 0;
}
void aimee_hook_ir_free(aimee_hook_ir_t *hook)
{
   if (!hook)
      return;
   aimee_block_free_contents(&hook->call);
   aimee_block_free_contents(&hook->result);
   free(hook->session_id);
   free(hook->cwd);
   free(hook->tool_cwd);
   memset(hook, 0, sizeof *hook);
}
static int hook_from_object(const cJSON *root, int native, aimee_hook_ir_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   if (!cJSON_IsObject(root))
   {
      return -1;
   }
   out->call.type = AIMEE_BLK_TOOL_USE;
   out->result.type = AIMEE_BLK_TOOL_RESULT;
   int rc = -1;
   if (text(root, "tool_name", &out->call.tool_name, 1) ||
       text(root, "tool_use_id", &out->call.tool_id, 0) ||
       text(root, "namespace", &out->call.tool_namespace, 0) ||
       text(root, "session_id", &out->session_id, 0) || cwd(root, &out->cwd))
      goto done;
   char *other = NULL;
   if (text(root, "tool_call_id", &other, 0))
      goto done;
   if (other && out->call.tool_id && strcmp(other, out->call.tool_id))
   {
      free(other);
      goto done;
   }
   if (!out->call.tool_id)
      out->call.tool_id = other;
   else
      free(other);
   other = NULL;
   if (text(root, "tool_namespace", &other, 0))
      goto done;
   if (other && out->call.tool_namespace && strcmp(other, out->call.tool_namespace))
   {
      free(other);
      goto done;
   }
   if (!out->call.tool_namespace)
      out->call.tool_namespace = other;
   else
      free(other);
   const cJSON *input = cJSON_GetObjectItemCaseSensitive(root, "tool_input");
   if (native && cJSON_IsString(input))
   {
      /* Decode a serialized argument object/array exactly once. Opaque string
       * inputs (including shell groups/tests) remain strings. A parsed object
       * with duplicate keys or trailing bytes must never fall back to text. */
      cJSON *candidate = cJSON_Parse(input->valuestring);
      if (cJSON_IsObject(candidate) || cJSON_IsArray(candidate))
         out->call.tool_input = parse_frame(input->valuestring, strlen(input->valuestring));
      else
         out->call.tool_input = cJSON_Duplicate(input, 1);
      cJSON_Delete(candidate);
      if (!out->call.tool_input)
         goto done;
   }
   else
      out->call.tool_input = input ? cJSON_Duplicate(input, 1) : cJSON_CreateObject();
   if (!out->call.tool_input)
      goto done;
   /* Check nested routing metadata as well, without silently truncating it or
    * conflating it with the outer transport cwd. Host routing owns precedence. */
   if (cJSON_IsObject(out->call.tool_input) && cwd(out->call.tool_input, &out->tool_cwd))
      goto done;
   const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "tool_response");
   if (result)
   {
      out->result.tool_result = cJSON_Duplicate(result, 1);
      if (!out->result.tool_result)
         goto done;
      if (out->call.tool_id)
      {
         out->result.tool_id = strdup(out->call.tool_id);
         if (!out->result.tool_id)
            goto done;
      }
      const cJSON *error = cJSON_GetObjectItemCaseSensitive(result, "is_error");
      const cJSON *native_error = cJSON_GetObjectItemCaseSensitive(result, "isError");
      if ((error && !cJSON_IsBool(error)) || (native_error && !cJSON_IsBool(native_error)) ||
          (error && native_error && cJSON_IsTrue(error) != cJSON_IsTrue(native_error)))
         goto done;
      out->result.tool_is_error = cJSON_IsTrue(error) || cJSON_IsTrue(native_error);
   }
   rc = 0;
done:
   if (rc)
      aimee_hook_ir_free(out);
   return rc;
}
int aimee_hook_ir_parse(const char *frame, size_t length, aimee_hook_ir_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   cJSON *root = parse_frame(frame, length);
   int rc = hook_from_object(root, 1, out);
   cJSON_Delete(root);
   return rc;
}
static int known_keys(const cJSON *object, const char *const *keys)
{
   if (!cJSON_IsObject(object))
      return 0;
   for (const cJSON *field = object->child; field; field = field->next)
   {
      int found = 0;
      for (size_t i = 0; keys[i]; i++)
         if (!strcmp(field->string, keys[i]))
            found = 1;
      if (!found)
         return 0;
   }
   return 1;
}
static int copy_field(cJSON *to, const char *name, const cJSON *value)
{
   if (!value)
      return 1;
   cJSON *copy = cJSON_Duplicate(value, 1);
   if (copy && cJSON_AddItemToObject(to, name, copy))
      return 1;
   cJSON_Delete(copy);
   return 0;
}
int aimee_hook_ir_admit(const cJSON *request, aimee_hook_ir_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   if (!cJSON_IsObject(request) || !aimee_ir_json_unique(request))
      return -1;
   const cJSON *version = cJSON_GetObjectItemCaseSensitive(request, "ir_version");
   if (!version)
      return hook_from_object(request, 1, out); /* explicit legacy entry */
   if (!cJSON_IsNumber(version) || version->valuedouble != 1 ||
       cJSON_GetObjectItemCaseSensitive(request, "tool_name") ||
       cJSON_GetObjectItemCaseSensitive(request, "tool_input") ||
       cJSON_GetObjectItemCaseSensitive(request, "tool_response") ||
       cJSON_GetObjectItemCaseSensitive(request, "tool_use_id") ||
       cJSON_GetObjectItemCaseSensitive(request, "tool_call_id") ||
       cJSON_GetObjectItemCaseSensitive(request, "namespace") ||
       cJSON_GetObjectItemCaseSensitive(request, "tool_namespace"))
      return -1;
   const cJSON *hook = cJSON_GetObjectItemCaseSensitive(request, "hook");
   const char *const hook_keys[] = {"call", "result", NULL};
   const char *const call_keys[] = {"type", "tool_id", "name", "namespace", "input", NULL};
   const char *const result_keys[] = {"type", "tool_id", "content", "is_error", NULL};
   const cJSON *call = cJSON_GetObjectItemCaseSensitive(hook, "call");
   const cJSON *kind = cJSON_GetObjectItemCaseSensitive(call, "type");
   const cJSON *input = cJSON_GetObjectItemCaseSensitive(call, "input");
   if (!known_keys(hook, hook_keys) || !known_keys(call, call_keys) || !cJSON_IsString(kind) ||
       strcmp(kind->valuestring, "tool_use") || !input)
      return -1;
   cJSON *projection = cJSON_CreateObject();
   int rc = -1;
   if (!projection ||
       !copy_field(projection, "tool_name", cJSON_GetObjectItemCaseSensitive(call, "name")) ||
       !copy_field(projection, "tool_use_id", cJSON_GetObjectItemCaseSensitive(call, "tool_id")) ||
       !copy_field(projection, "namespace", cJSON_GetObjectItemCaseSensitive(call, "namespace")) ||
       !copy_field(projection, "tool_input", input) ||
       !copy_field(projection, "session_id",
                   cJSON_GetObjectItemCaseSensitive(request, "session_id")) ||
       !copy_field(projection, "cwd", cJSON_GetObjectItemCaseSensitive(request, "cwd")) ||
       hook_from_object(projection, 0, out))
      goto done;
   const cJSON *result = cJSON_GetObjectItemCaseSensitive(hook, "result");
   if (result)
   {
      kind = cJSON_GetObjectItemCaseSensitive(result, "type");
      const cJSON *id = cJSON_GetObjectItemCaseSensitive(result, "tool_id");
      const cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
      const cJSON *error = cJSON_GetObjectItemCaseSensitive(result, "is_error");
      if (!known_keys(result, result_keys) || !cJSON_IsString(kind) ||
          strcmp(kind->valuestring, "tool_result") || !content || !cJSON_IsBool(error) ||
          (id && (!cJSON_IsString(id) || !out->call.tool_id ||
                  strcmp(id->valuestring, out->call.tool_id))))
         goto done;
      if (id && !(out->result.tool_id = strdup(id->valuestring)))
         goto done;
      out->result.tool_result = cJSON_Duplicate(content, 1);
      if (!out->result.tool_result)
         goto done;
      out->result.tool_is_error = cJSON_IsTrue(error);
   }
   rc = 0;
done:
   cJSON_Delete(projection);
   if (rc)
      aimee_hook_ir_free(out);
   return rc;
}
cJSON *aimee_hook_ir_json(const aimee_hook_ir_t *hook)
{
   if (!hook || !hook->call.tool_name || !hook->call.tool_name[0] || !hook->call.tool_input)
      return NULL;
   cJSON *out = cJSON_CreateObject();
   cJSON *call = out ? cJSON_AddObjectToObject(out, "call") : NULL;
   if (!call || !cJSON_AddStringToObject(call, "type", "tool_use") ||
       !cJSON_AddStringToObject(call, "name", hook->call.tool_name) ||
       (hook->call.tool_id && !cJSON_AddStringToObject(call, "tool_id", hook->call.tool_id)) ||
       (hook->call.tool_namespace &&
        !cJSON_AddStringToObject(call, "namespace", hook->call.tool_namespace)) ||
       !copy_field(call, "input", hook->call.tool_input))
      goto fail;
   if (hook->result.tool_result)
   {
      cJSON *result = cJSON_AddObjectToObject(out, "result");
      if (!result || !cJSON_AddStringToObject(result, "type", "tool_result") ||
          (hook->result.tool_id &&
           !cJSON_AddStringToObject(result, "tool_id", hook->result.tool_id)) ||
          !copy_field(result, "content", hook->result.tool_result) ||
          !cJSON_AddBoolToObject(result, "is_error", hook->result.tool_is_error))
         goto fail;
   }
   return out;
fail:
   cJSON_Delete(out);
   return NULL;
}

char *aimee_hook_ir_arguments(const aimee_hook_ir_t *hook)
{
   if (!hook || !hook->call.tool_input)
      return NULL;
   return cJSON_IsString(hook->call.tool_input) ? strdup(hook->call.tool_input->valuestring)
                                                : cJSON_PrintUnformatted(hook->call.tool_input);
}
