#include "aimee.h"
#include "agent_tools_internal.h"
#include "modules/workspace/workspace_provider.h"
#include <aimee/core/turn_integrity.h>
#include <aimee/tools/agent_tools.h>
#include <aimee/tools/module_api.h>

static __thread ti_effect_contract_t g_effect;
static __thread int g_effect_active;
static __thread int g_effect_authorized;

void agent_tools_set_effect_authorized(int authorized)
{
   g_effect_authorized = !!authorized;
}

void agent_tools_effect_reset(void)
{
   memset(&g_effect, 0, sizeof g_effect);
   g_effect_active = 0;
}

int agent_tools_effect_classification(const char *name, int (*classifier)(const char *, int *))
{
   int classification = AIMEE_TOOL_CLASS_UNKNOWN;
   if (classifier)
      (void)classifier(name, &classification);
   return classification;
}

int agent_tools_effect_mcp_failure_is_timeout(const char *error)
{
   return error &&
          (strcmp(error, "transport timeout") == 0 || strcmp(error, "request timed out") == 0);
}

static ti_effect_class_t effect_class(const char *name, int classification)
{
   if (name && (strcmp(name, "web_search") == 0 || strcmp(name, "web_read") == 0 ||
                strcmp(name, "git_push") == 0 || strcmp(name, "git_pr") == 0 || strchr(name, ':')))
      return TI_EFFECT_EXTERNAL_COMMUNICATION;
   switch (classification)
   {
   case AIMEE_TOOL_CLASS_READ:
   case AIMEE_TOOL_CLASS_CONTROL:
      return TI_EFFECT_READ_ONLY;
   case AIMEE_TOOL_CLASS_WRITE:
      return TI_EFFECT_REVERSIBLE;
   case AIMEE_TOOL_CLASS_EXEC:
      return TI_EFFECT_CONDITIONALLY_REVERSIBLE;
   case AIMEE_TOOL_CLASS_REMOTE:
      return TI_EFFECT_EXTERNAL_COMMUNICATION;
   default:
      return TI_EFFECT_UNCLASSIFIED;
   }
}

static int external_mutation(const char *name)
{
   return name &&
          (strcmp(name, "git_push") == 0 || strcmp(name, "git_pr") == 0 || strchr(name, ':'));
}

static const char *effect_target(const char *name, cJSON *args)
{
   static const char *const keys[] = {"path", "repo_path", "url", "ref", "title", "symbol", NULL};
   for (int i = 0; keys[i]; i++)
   {
      cJSON *value = cJSON_GetObjectItemCaseSensitive(args, keys[i]);
      if (cJSON_IsString(value))
         return value->valuestring;
   }
   return external_mutation(name) ? name : "";
}

static int known_reversible(const char *name, cJSON *args)
{
   if (!name || !args)
      return 0;
   if (strcmp(name, "write_file") == 0)
      return 1;
   if (strcmp(name, "edit_file") == 0)
   {
      cJSON *dry_run = cJSON_GetObjectItemCaseSensitive(args, "dry_run");
      return !(cJSON_IsBool(dry_run) && cJSON_IsTrue(dry_run));
   }
   if (strcmp(name, "edit_symbol") == 0)
   {
      cJSON *path = cJSON_GetObjectItemCaseSensitive(args, "path");
      return cJSON_IsString(path) && path->valuestring[0];
   }
   return 0;
}

static ti_effect_mode_t effect_mode(const char *name, cJSON *args)
{
   const char *configured = getenv("AIMEE_EFFECT_CONTRACT_MODE");
   if (configured && strcmp(configured, "off") == 0)
      return TI_EFFECT_MODE_OFF;
   if (configured && strcmp(configured, "shadow") == 0)
      return TI_EFFECT_MODE_SHADOW;
   return (known_reversible(name, args) || external_mutation(name)) ? TI_EFFECT_MODE_ENFORCE
                                                                    : TI_EFFECT_MODE_SHADOW;
}

void agent_tools_effect_propose(const char *name, cJSON *args, int classification)
{
   char *normalized = cJSON_PrintUnformatted(args);
   const char *who = session_id();
   ti_effect_class_t cls =
       known_reversible(name, args) ? TI_EFFECT_REVERSIBLE : effect_class(name, classification);
   if (ti_effect_contract_init(&g_effect, who, name, effect_target(name, args), normalized, cls,
                               effect_mode(name, args)) == 0)
   {
      g_effect_active = 1;
      if (external_mutation(name))
      {
         (void)ti_effect_contract_set_authorization(&g_effect, 1, g_effect_authorized);
         ti_idempotency_t idempotency =
             strcmp(name, "git_push") == 0
                 ? TI_IDEMPOTENT
                 : (strcmp(name, "git_pr") == 0 ? TI_NON_IDEMPOTENT : TI_IDEMPOTENCY_UNKNOWN);
         (void)ti_effect_contract_set_idempotency(&g_effect, idempotency);
      }
      else if (g_effect.effect_class == TI_EFFECT_EXTERNAL_COMMUNICATION)
         (void)ti_effect_contract_set_idempotency(&g_effect, TI_IDEMPOTENT);
      if (g_effect.mode == TI_EFFECT_MODE_ENFORCE && known_reversible(name, args))
         (void)ti_effect_contract_require_postcondition(&g_effect);
   }
   free(normalized);
}

int agent_tools_effect_validate_and_execute(const char *name, cJSON *args, int classification)
{
   if (!g_effect_active)
      return 0;
   if (g_effect.mode == TI_EFFECT_MODE_ENFORCE && !effect_target(name, args)[0])
      return -1;
   char *normalized = cJSON_PrintUnformatted(args);
   ti_effect_class_t cls =
       known_reversible(name, args) ? TI_EFFECT_REVERSIBLE : effect_class(name, classification);
   int matched =
       ti_effect_contract_validate(&g_effect, name, effect_target(name, args), normalized, cls);
   free(normalized);
   if (matched < 0 || (matched == 0 && g_effect.mode == TI_EFFECT_MODE_ENFORCE))
      return -1;
   return ti_effect_contract_mark_executing(&g_effect);
}

int agent_tools_effect_postcondition_pending(void)
{
   return g_effect_active && g_effect.postcondition == TI_POSTCONDITION_PENDING;
}

int agent_tools_effect_verify_file_postcondition(const char *name, cJSON *args,
                                                 const char *dispatch_cwd)
{
   if (!g_effect_active || g_effect.mode != TI_EFFECT_MODE_ENFORCE)
      return 1;
   cJSON *path = cJSON_GetObjectItemCaseSensitive(args, "path");
   if (!cJSON_IsString(path) || !path->valuestring[0])
      return 0;
   char absolute[MAX_PATH_LEN];
   normalize_path(path->valuestring, dispatch_cwd, absolute, sizeof absolute);
   const workspace_provider_t *provider = workspace_provider_active();
   char *actual = NULL;
   size_t actual_len = 0;
   if (!provider || provider->read_all(provider, absolute, &actual, &actual_len) != 0 || !actual)
   {
      free(actual);
      return 0;
   }

   int passed = 1;
   if (strcmp(name, "write_file") == 0)
   {
      cJSON *content = cJSON_GetObjectItemCaseSensitive(args, "content");
      const char *expected = cJSON_IsString(content) ? content->valuestring : "";
      size_t expected_len = strlen(expected);
      passed = actual_len == expected_len && memcmp(actual, expected, expected_len) == 0;
   }
   else if (strcmp(name, "edit_file") == 0)
   {
      cJSON *old = cJSON_GetObjectItemCaseSensitive(args, "old_string");
      cJSON *replacement = cJSON_GetObjectItemCaseSensitive(args, "new_string");
      if (cJSON_IsString(old))
      {
         const char *new_text = cJSON_IsString(replacement) ? replacement->valuestring : "";
         if (strcmp(old->valuestring, new_text) == 0)
            passed = strstr(actual, old->valuestring) != NULL;
         else
            passed = strstr(actual, old->valuestring) == NULL &&
                     (!new_text[0] || strstr(actual, new_text) != NULL);
      }
   }
   else if (strcmp(name, "edit_symbol") == 0)
   {
      cJSON *text = cJSON_GetObjectItemCaseSensitive(args, "text");
      cJSON *op = cJSON_GetObjectItemCaseSensitive(args, "op");
      if (cJSON_IsString(text) && text->valuestring[0] &&
          !(cJSON_IsString(op) && strcmp(op->valuestring, "delete") == 0))
         passed = strstr(actual, text->valuestring) != NULL;
   }
   free(actual);
   return passed;
}

int agent_tools_effect_result_claims_success(const char *result)
{
   if (!result || strncmp(result, "error:", 6) == 0)
      return 0;
   cJSON *payload = cJSON_Parse(result);
   if (!payload)
      return 1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(payload, "status");
   int succeeded = !cJSON_IsString(status) || strcmp(status->valuestring, "ok") == 0;
   cJSON_Delete(payload);
   return succeeded;
}

void agent_tools_effect_record_postcondition(int passed, const char *detail)
{
   if (g_effect_active)
      (void)ti_effect_contract_record_postcondition(&g_effect, passed, detail);
}

void agent_tools_effect_finish(const char *verdict, const char *reason)
{
   if (g_effect_active)
   {
      ti_effect_state_t outcome = TI_EFFECT_FAILED;
      if (strcmp(verdict, "ok") == 0)
         outcome = TI_EFFECT_SUCCEEDED;
      else if (strcmp(verdict, "refused") == 0)
         outcome = TI_EFFECT_REFUSED;
      else if (strcmp(verdict, "timeout") == 0 &&
               g_effect.effect_class == TI_EFFECT_EXTERNAL_COMMUNICATION &&
               g_effect.idempotency != TI_IDEMPOTENT)
         outcome = TI_EFFECT_UNKNOWN_OUTCOME;
      (void)ti_effect_contract_finish(&g_effect, outcome, reason);
      g_effect_active = 0;
   }
   g_effect_authorized = 0;
}
