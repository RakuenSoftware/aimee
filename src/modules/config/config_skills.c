/* config_skills.c: parser for the `skills` config section. */
#include "aimee.h"
#include "config.h"
#include "json_fluent.h"
#include "cJSON.h"

#include <stdarg.h>
#include <stdio.h>

static int skills_int_field(int *dst, const cJSON *root, const char *key, const char *path)
{
   const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
   if (!item)
      return 0;
   if (!cJSON_IsNumber(item) || item->valueint <= 0)
      return config_issue("\"%s\" must be a positive integer, got %s", path, jo_type_name(item));
   *dst = item->valueint;
   return 0;
}

static int skills_double_field(double *dst, const cJSON *root, const char *key, const char *path)
{
   const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
   if (!item)
      return 0;
   if (!cJSON_IsNumber(item) || item->valuedouble < 0.0)
      return config_issue("\"%s\" must be a non-negative number, got %s", path, jo_type_name(item));
   *dst = item->valuedouble;
   return 0;
}

int config_parse_skills(config_t *cfg, const cJSON *root)
{
   const cJSON *skills = cJSON_GetObjectItemCaseSensitive(root, "skills");
   if (!skills)
      return 0;
   if (!cJSON_IsObject(skills))
      return config_issue("\"skills\" expected object, got %s", jo_type_name(skills));

   int issues = 0;
   const cJSON *review = cJSON_GetObjectItemCaseSensitive(skills, "review");
   if (review && cJSON_IsObject(review))
   {
      const cJSON *en = cJSON_GetObjectItemCaseSensitive(review, "enabled");
      if (en)
         cfg->skills_review_enabled = cJSON_IsTrue(en) ? 1 : 0;
   }
   issues += skills_int_field(&cfg->skills_review_nudge_interval, skills, "review_nudge_interval",
                              "skills.review_nudge_interval");
   issues += skills_int_field(&cfg->skills_stale_after_days, skills, "stale_after_days",
                              "skills.stale_after_days");
   issues += skills_int_field(&cfg->skills_archive_after_days, skills, "archive_after_days",
                              "skills.archive_after_days");
   const cJSON *dispatch = cJSON_GetObjectItemCaseSensitive(skills, "dispatch");
   if (dispatch)
   {
      if (!cJSON_IsObject(dispatch))
         issues +=
             config_issue("\"skills.dispatch\" expected object, got %s", jo_type_name(dispatch));
      else
      {
         const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(dispatch, "enabled");
         if (enabled)
         {
            if (!cJSON_IsBool(enabled))
               issues += config_issue("\"skills.dispatch.enabled\" expected boolean, got %s",
                                      jo_type_name(enabled));
            else
               cfg->skills_dispatch_enabled = cJSON_IsTrue(enabled) ? 1 : 0;
         }
         issues += skills_int_field(&cfg->skills_dispatch_max_index, dispatch, "max_index",
                                    "skills.dispatch.max_index");
         const cJSON *advisory = cJSON_GetObjectItemCaseSensitive(dispatch, "advisory");
         if (advisory)
         {
            if (!cJSON_IsBool(advisory))
               issues += config_issue("\"skills.dispatch.advisory\" expected boolean, got %s",
                                      jo_type_name(advisory));
            else
               cfg->skills_dispatch_advisory = cJSON_IsTrue(advisory) ? 1 : 0;
         }
      }
   }
   const cJSON *capability = cJSON_GetObjectItemCaseSensitive(skills, "capability");
   if (capability)
   {
      if (!cJSON_IsObject(capability))
         issues += config_issue("\"skills.capability\" expected object, got %s",
                                jo_type_name(capability));
      else
      {
         const cJSON *autostub = cJSON_GetObjectItemCaseSensitive(capability, "autostub");
         if (autostub)
         {
            if (!cJSON_IsBool(autostub))
               issues += config_issue("\"skills.capability.autostub\" expected boolean, got %s",
                                      jo_type_name(autostub));
            else
               cfg->skills_capability_autostub = cJSON_IsTrue(autostub) ? 1 : 0;
         }
      }
   }
   const cJSON *eval = cJSON_GetObjectItemCaseSensitive(skills, "eval");
   if (eval)
   {
      if (!cJSON_IsObject(eval))
         issues += config_issue("\"skills.eval\" expected object, got %s", jo_type_name(eval));
      else
      {
         const cJSON *gate = cJSON_GetObjectItemCaseSensitive(eval, "gate_enabled");
         if (gate)
         {
            if (!cJSON_IsBool(gate))
               issues += config_issue("\"skills.eval.gate_enabled\" expected boolean, got %s",
                                      jo_type_name(gate));
            else
               cfg->skills_eval_gate_enabled = cJSON_IsTrue(gate) ? 1 : 0;
         }
         issues += skills_double_field(&cfg->skills_eval_threshold, eval, "threshold",
                                       "skills.eval.threshold");
      }
   }
   return issues;
}

void config_computer_use_defaults(config_t *cfg)
{
   cfg->computer_use_enabled = 0;
   snprintf(cfg->computer_use_default_navigation, sizeof(cfg->computer_use_default_navigation),
            "%s", "approve");
   cfg->computer_use_redact_sensitive_screenshots = 1;
   snprintf(cfg->computer_use_allowed_domains[0], sizeof(cfg->computer_use_allowed_domains[0]),
            "%s", "localhost");
   cfg->computer_use_allowed_domain_count = 1;
}

int config_parse_computer_use(config_t *cfg, const cJSON *root)
{
   const cJSON *cu = cJSON_GetObjectItemCaseSensitive(root, "computer_use");
   if (!cJSON_IsObject(cu))
      return 0;

   const cJSON *item = cJSON_GetObjectItemCaseSensitive(cu, "enabled");
   if (cJSON_IsBool(item))
      cfg->computer_use_enabled = cJSON_IsTrue(item) ? 1 : 0;
   item = cJSON_GetObjectItemCaseSensitive(cu, "default_navigation");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->computer_use_default_navigation, sizeof(cfg->computer_use_default_navigation),
               "%s", item->valuestring);
   item = cJSON_GetObjectItemCaseSensitive(cu, "redact_sensitive_screenshots");
   if (cJSON_IsBool(item))
      cfg->computer_use_redact_sensitive_screenshots = cJSON_IsTrue(item) ? 1 : 0;

   const cJSON *domains = cJSON_GetObjectItemCaseSensitive(cu, "allowed_domains");
   if (cJSON_IsArray(domains))
   {
      int i = 0;
      const cJSON *el;
      cJSON_ArrayForEach(el, domains)
      {
         if (i >= CONFIG_COMPUTER_USE_MAX_DOMAINS)
            break;
         if (!cJSON_IsString(el) || !el->valuestring[0])
            continue;
         snprintf(cfg->computer_use_allowed_domains[i],
                  sizeof(cfg->computer_use_allowed_domains[0]), "%s", el->valuestring);
         i++;
      }
      cfg->computer_use_allowed_domain_count = i;
   }
   return 0;
}
