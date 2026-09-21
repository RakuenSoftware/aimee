/* Generic executor for plans produced by an authenticated module. The module
 * selects effects, resources and context metadata; the host owns IR allocation
 * and explicitly supplied connections. No domain decisions belong here. */
#include <aimee/ir/module_plan.h>
#include "module_commands.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PLAN_TEXT_CAPACITY 65536
#define PLAN_STEP_LIMIT    64

static const char *string_field(const cJSON *object, const char *key)
{
   return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
}
static const aimee_ir_plan_binding_t *binding(const aimee_ir_module_plan_t *config,
                                              const char *name)
{
   if (name && config->bindings)
      for (const aimee_ir_plan_binding_t *b = config->bindings; b->name; b++)
         if (strcmp(b->name, name) == 0 && b->invoke)
            return b;
   return NULL;
}
static const char *resource(const aimee_ir_module_plan_t *config, const char *name)
{
   if (name && config->resources)
      for (const aimee_ir_plan_resource_t *r = config->resources; r->name; r++)
         if (strcmp(r->name, name) == 0)
            return r->text;
   return NULL;
}
static int enum_value(const char *value, const char *const *names, int count)
{
   if (value)
      for (int i = 0; i < count; i++)
         if (strcmp(value, names[i]) == 0)
            return i;
   return -1;
}
static int context_decode(const cJSON *json, aimee_context_meta_t *out)
{
   static const char *const origins[] = {"unknown", "platform",  "operator", "user",
                                         "tool",    "retrieval", "memory",   "model"};
   static const char *const authorities[] = {"diagnostic", "evidence", "task_instruction",
                                             "policy"};
   static const char *const trusts[] = {"unknown", "untrusted", "unverified", "reviewed",
                                        "verified"};
   static const char *const sensitivities[] = {"public", "internal", "confidential", "restricted"};
   int origin = enum_value(string_field(json, "origin"), origins, 8);
   int authority = enum_value(string_field(json, "authority"), authorities, 4);
   int trust = enum_value(string_field(json, "trust"), trusts, 5);
   int sensitivity = enum_value(string_field(json, "sensitivity"), sensitivities, 4);
   const cJSON *visible = cJSON_GetObjectItemCaseSensitive(json, "model_visible");
   if (!cJSON_IsObject(json) || origin < 0 || authority < 0 || trust < 0 || sensitivity < 0 ||
       !cJSON_IsBool(visible))
      return -1;
   if (!aimee_ir_context_promotion_allowed(origin, AIMEE_CTX_AUTH_DIAGNOSTIC, authority))
      return -1;
   memset(out, 0, sizeof(*out));
   out->origin = origin;
   out->authority = authority;
   out->trust = trust;
   out->sensitivity = sensitivity;
   out->model_visible = cJSON_IsTrue(visible);
   const char *domain = string_field(json, "revision_domain"),
              *scope = string_field(json, "revision_scope");
   if ((domain && strlen(domain) >= sizeof(out->revision_domain)) ||
       (scope && strlen(scope) >= sizeof(out->revision_scope)))
      return -1;
   if (domain)
      strcpy(out->revision_domain, domain);
   if (scope)
      strcpy(out->revision_scope, scope);
   return 0;
}
static int reference_valid(const cJSON *step, const char *key)
{
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(step, key);
   return !v || (cJSON_IsString(v) && v->valuestring[0] && strlen(v->valuestring) < 64);
}
static int validate_plan(const aimee_request_t *ir, const cJSON *steps,
                         const aimee_ir_module_plan_t *config)
{
   if (!cJSON_IsArray(steps) || cJSON_GetArraySize(steps) > PLAN_STEP_LIMIT)
      return -1;
   int tool_patch = 0;
   const cJSON *step;
   cJSON_ArrayForEach(step, steps)
   {
      const char *kind = string_field(step, "kind");
      if (!kind || !reference_valid(step, "output") || !reference_valid(step, "when_output") ||
          !reference_valid(step, "epoch_output"))
         return -1;
      if (strcmp(kind, "invoke") == 0)
      {
         if (!binding(config, string_field(step, "binding")) ||
             !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(step, "args")))
            return -1;
      }
      else if (strcmp(kind, "append_context") == 0)
      {
         aimee_context_meta_t metadata;
         const char *name = string_field(step, "resource"), *output = string_field(step, "output");
         if (!ir || (name && output) || (!name && !output) || (name && !resource(config, name)) ||
             context_decode(cJSON_GetObjectItemCaseSensitive(step, "context"), &metadata) != 0)
            return -1;
      }
      else if (strcmp(kind, "remove_tools") == 0)
      {
         if (!ir || tool_patch++)
            return -1;
         const cJSON *indices = cJSON_GetObjectItemCaseSensitive(step, "indices"), *index;
         if (!cJSON_IsArray(indices))
            return -1;
         int upper = ir->n_tools;
         cJSON_ArrayForEach(index, indices)
         {
            if (!cJSON_IsNumber(index) || index->valuedouble < 0 || index->valuedouble >= upper ||
                index->valuedouble != (double)index->valueint)
               return -1;
            upper = index->valueint;
         }
      }
      else
         return -1;
   }
   return 0;
}
static cJSON *request_plan(const aimee_request_t *ir, const aimee_ir_module_plan_t *config)
{
   if (!config || !config->method || !config->operation || !config->phase)
      return NULL;
   cJSON *args = cJSON_CreateObject(), *reply = NULL;
   if (!args || !cJSON_AddStringToObject(args, "operation", config->operation) ||
       !cJSON_AddStringToObject(args, "phase", config->phase))
      goto done;
   if (config->provided_query &&
       !cJSON_AddStringToObject(args, "provided_query", config->provided_query))
      goto done;
   if (config->provided_resources)
   {
      cJSON *provided = cJSON_AddArrayToObject(args, "provided_resources");
      if (!provided)
         goto done;
      for (const char *const *name = config->provided_resources; *name; name++)
         if (!cJSON_AddItemToArray(provided, cJSON_CreateString(*name)))
            goto done;
   }
   if (ir)
   {
      cJSON *roles = cJSON_AddArrayToObject(args, "roles"),
            *tools = cJSON_AddArrayToObject(args, "tools");
      if (!roles || !tools)
         goto done;
      for (int i = 0; i < ir->n_messages; i++)
         if (!cJSON_AddItemToArray(
                 roles, cJSON_CreateString(ir->messages[i].role ? ir->messages[i].role : "")))
            goto done;
      for (int i = 0; i < ir->n_tools; i++)
         if (!cJSON_AddItemToArray(tools,
                                   cJSON_CreateString(ir->tools[i].name ? ir->tools[i].name : "")))
            goto done;
      char *text = malloc(PLAN_TEXT_CAPACITY);
      if (!text)
         goto done;
      aimee_ir_last_user_text(ir, text, PLAN_TEXT_CAPACITY);
      cJSON *added = cJSON_AddStringToObject(args, "last_user_text", text);
      free(text);
      if (!added)
         goto done;
   }
   if (aimee_module_commands_dispatch_internal_timeout(config->method, args, 500, &reply) <= 0)
   {
      cJSON_Delete(reply);
      reply = NULL;
   }
done:
   cJSON_Delete(args);
   return reply;
}
static int output_present(const cJSON *outputs, const char *name)
{
   if (!name)
      return 1;
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(outputs, name);
   return value && !cJSON_IsNull(value) && (!cJSON_IsString(value) || value->valuestring[0]);
}
static int append_context(aimee_request_t *ir, const cJSON *step, const cJSON *outputs,
                          const aimee_ir_module_plan_t *config)
{
   const char *name = string_field(step, "resource");
   const char *text =
       name ? resource(config, name) : string_field(outputs, string_field(step, "output"));
   if (!text || !text[0])
      return 0;
   aimee_context_meta_t metadata;
   if (context_decode(cJSON_GetObjectItemCaseSensitive(step, "context"), &metadata) != 0)
      return 0;
   const char *epoch_name = string_field(step, "epoch_output");
   if (epoch_name)
   {
      const char *epoch = string_field(outputs, epoch_name);
      if (epoch)
      {
         char *end = NULL;
         if (!epoch[0] || strspn(epoch, "0123456789") != strlen(epoch))
            return 0;
         errno = 0;
         unsigned long long value = strtoull(epoch, &end, 10);
         if (errno || !end || *end)
            return 0;
         metadata.revision_epoch = value;
      }
   }
   char *owned = strdup(text);
   if (!owned)
      return 0;
   aimee_block_t *grown = realloc(ir->system, (size_t)(ir->n_system + 1) * sizeof(*grown));
   if (!grown)
   {
      free(owned);
      return 0;
   }
   ir->system = grown;
   aimee_block_t *block = &ir->system[ir->n_system++];
   memset(block, 0, sizeof(*block));
   block->type = AIMEE_BLK_TEXT;
   block->text = owned;
   block->context = metadata;
   return 1;
}
static int execute(aimee_request_t *ir, const aimee_ir_module_plan_t *config, char **text_result)
{
   if (text_result)
      *text_result = NULL;
   cJSON *plan = request_plan(ir, config), *outputs = cJSON_CreateObject();
   const char *status = string_field(plan, "status");
   const cJSON *steps = cJSON_GetObjectItemCaseSensitive(plan, "steps");
   int changed = 0;
   if (!outputs || !status || strcmp(status, "ok") != 0 || validate_plan(ir, steps, config) != 0)
   {
      if (config && config->refuse)
         config->refuse(string_field(plan, "kind"), config->context);
      goto done;
   }
   const cJSON *step;
   cJSON_ArrayForEach(step, steps)
   {
      if (!output_present(outputs, string_field(step, "when_output")))
         continue;
      const char *kind = string_field(step, "kind");
      if (strcmp(kind, "invoke") == 0)
      {
         const aimee_ir_plan_binding_t *b = binding(config, string_field(step, "binding"));
         cJSON *value = b->invoke(cJSON_GetObjectItemCaseSensitive(step, "args"), config->context);
         const char *output = string_field(step, "output");
         if (output)
         {
            cJSON_DeleteItemFromObjectCaseSensitive(outputs, output);
            if (!value)
               value = cJSON_CreateNull();
            if (!cJSON_AddItemToObject(outputs, output, value))
               cJSON_Delete(value);
         }
         else
            cJSON_Delete(value);
      }
      else if (strcmp(kind, "append_context") == 0)
         changed |= append_context(ir, step, outputs, config);
      else if (aimee_ir_remove_tools(ir, cJSON_GetObjectItemCaseSensitive(step, "indices")) > 0)
         changed = 1;
   }
   if (text_result)
   {
      const char *key = string_field(plan, "result");
      const char *value = key ? string_field(outputs, key) : NULL;
      if (value && value[0])
         *text_result = strdup(value);
   }
done:
   cJSON_Delete(outputs);
   cJSON_Delete(plan);
   return changed;
}
int aimee_ir_stage_module_plan(aimee_request_t *request, void *configuration)
{
   if (!request)
      return 0;
   return execute(request, configuration, NULL);
}
char *aimee_ir_module_plan_text(const aimee_ir_module_plan_t *configuration)
{
   char *text = NULL;
   execute(NULL, configuration, &text);
   return text;
}
