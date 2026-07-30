/* config_trigger.c: parser for the top-level `trigger` YAML section and the
 * `trigger_rules` list.  Split out of config.c to keep that file under the
 * line-count gate.
 *
 * trigger:
 *   auth_token: "secret"
 *   max_concurrent: 2
 *
 * trigger_rules:
 *   - source: "github-webhook"   # valid sources include github-webhook, cron,
 *                                # watch-dir (alias: proposals)
 *     event: "push"
 *     mode: "autonomous"         # "autonomous" (default) | "interactive"
 *     pipeline:
 *       template: "ci"
 *       workspace: "/ws/myproject"
 *       max_spend_usd: 0.50
 *
 * source: "proposals" reuses existing fields: workspace is the absolute git
 * repo to scan, pipeline.template is the workflow name, event is the
 * repo-relative proposal dir (default docs/proposals/pending), and schedule is
 * the git ref/branch (default auto-detect).
 *
 * mode selects how the filed work item runs, independent of the workflow it
 * names: "autonomous" (the default) lets the autonomy scheduler drive the run
 * hands-off — appropriate when the trigger event *is* the approval (e.g. a
 * merged proposal); "interactive" files the item and parks it for a human to
 * drive in the webchat. Gating BEYOND this (e.g. a gate.human step) is a
 * property of the workflow the rule names, not of the trigger.
 */

#include "aimee.h"
#include "config.h"
#include "json_fluent.h"
#include "cJSON.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int trigger_copy_string(char *dst, size_t dst_len, const cJSON *item, const char *path,
                               int required)
{
   if (!item)
   {
      return required ? config_issue("\"%s\" is required", path) : 0;
   }
   if (!cJSON_IsString(item) || !item->valuestring[0])
      return config_issue("\"%s\" expected non-empty string, got %s", path, jo_type_name(item));
   snprintf(dst, dst_len, "%s", item->valuestring);
   return 0;
}

static int cron_mode_valid(const char *mode)
{
   return strcmp(mode, "llm") == 0 || strcmp(mode, "script") == 0 || strcmp(mode, "hybrid") == 0;
}

static int config_parse_cron_job_deliver(cron_job_t *job, const cJSON *deliver, int index)
{
   if (!deliver)
      return 0;
   if (!cJSON_IsObject(deliver))
      return config_issue("\"cron_jobs[%d].deliver\" expected object, got %s", index,
                          jo_type_name(deliver));

   int issues = 0;
   const cJSON *target = cJSON_GetObjectItemCaseSensitive(deliver, "target");
   issues += trigger_copy_string(job->deliver_target, sizeof(job->deliver_target), target,
                                 "cron_jobs[].deliver.target", 0);

   const cJSON *only_changed = cJSON_GetObjectItemCaseSensitive(deliver, "only_if_changed");
   if (only_changed)
   {
      if (!cJSON_IsBool(only_changed))
         issues +=
             config_issue("\"cron_jobs[%d].deliver.only_if_changed\" expected boolean, got %s",
                          index, jo_type_name(only_changed));
      else
         job->deliver_only_if_changed = cJSON_IsTrue(only_changed) ? 1 : 0;
   }

   const cJSON *first_run_silent = cJSON_GetObjectItemCaseSensitive(deliver, "first_run_silent");
   if (first_run_silent)
   {
      if (!cJSON_IsBool(first_run_silent))
         issues +=
             config_issue("\"cron_jobs[%d].deliver.first_run_silent\" expected boolean, got %s",
                          index, jo_type_name(first_run_silent));
      else
         job->deliver_first_run_silent = cJSON_IsTrue(first_run_silent) ? 1 : 0;
   }
   return issues;
}

static int config_parse_cron_job_skills(cron_job_t *job, const cJSON *skills, int index)
{
   if (!skills)
      return 0;
   if (!cJSON_IsArray(skills))
      return config_issue("\"cron_jobs[%d].skills\" expected array, got %s", index,
                          jo_type_name(skills));

   int issues = 0;
   const cJSON *skill;
   cJSON_ArrayForEach(skill, skills)
   {
      if (job->skill_count >= CRON_JOB_MAX_SKILLS)
      {
         issues += config_issue("\"cron_jobs[%d].skills\" has too many entries (max %d)", index,
                                CRON_JOB_MAX_SKILLS);
         break;
      }
      if (!cJSON_IsString(skill) || !skill->valuestring[0])
      {
         issues += config_issue("\"cron_jobs[%d].skills[]\" expected non-empty string, got %s",
                                index, jo_type_name(skill));
         continue;
      }
      snprintf(job->skills[job->skill_count], sizeof(job->skills[job->skill_count]), "%s",
               skill->valuestring);
      job->skill_count++;
   }
   return issues;
}

static int config_parse_cron_jobs(config_t *cfg, const cJSON *root)
{
   const cJSON *jobs = cJSON_GetObjectItemCaseSensitive(root, "cron_jobs");
   if (!jobs)
      return 0;
   if (!cJSON_IsArray(jobs))
      return config_issue("\"cron_jobs\" expected array, got %s", jo_type_name(jobs));

   int issues = 0;
   int count = 0;
   const cJSON *item;
   cJSON_ArrayForEach(item, jobs)
   {
      if (count >= CRON_JOBS_MAX)
      {
         issues += config_issue("too many cron_jobs (max %d)", CRON_JOBS_MAX);
         break;
      }
      if (!cJSON_IsObject(item))
      {
         issues +=
             config_issue("\"cron_jobs[%d]\" expected object, got %s", count, jo_type_name(item));
         continue;
      }

      cron_job_t *job = &cfg->cron_jobs[count];
      memset(job, 0, sizeof(*job));
      job->enabled = 1;
      snprintf(job->mode, sizeof(job->mode), "%s", "llm");

      issues +=
          trigger_copy_string(job->id, sizeof(job->id),
                              cJSON_GetObjectItemCaseSensitive(item, "id"), "cron_jobs[].id", 1);
      issues += trigger_copy_string(job->schedule, sizeof(job->schedule),
                                    cJSON_GetObjectItemCaseSensitive(item, "schedule"),
                                    "cron_jobs[].schedule", 1);

      const cJSON *mode = cJSON_GetObjectItemCaseSensitive(item, "mode");
      if (mode)
      {
         issues += trigger_copy_string(job->mode, sizeof(job->mode), mode, "cron_jobs[].mode", 0);
         if (!cron_mode_valid(job->mode))
            issues += config_issue("\"cron_jobs[%d].mode\" expected llm, script, or hybrid", count);
      }

      issues += trigger_copy_string(job->script, sizeof(job->script),
                                    cJSON_GetObjectItemCaseSensitive(item, "script"),
                                    "cron_jobs[].script", 0);
      issues += trigger_copy_string(job->prompt, sizeof(job->prompt),
                                    cJSON_GetObjectItemCaseSensitive(item, "prompt"),
                                    "cron_jobs[].prompt", 0);
      issues += trigger_copy_string(job->workdir, sizeof(job->workdir),
                                    cJSON_GetObjectItemCaseSensitive(item, "workdir"),
                                    "cron_jobs[].workdir", 0);
      issues += trigger_copy_string(job->context_from, sizeof(job->context_from),
                                    cJSON_GetObjectItemCaseSensitive(item, "context_from"),
                                    "cron_jobs[].context_from", 0);
      issues += trigger_copy_string(job->when_context_contains, sizeof(job->when_context_contains),
                                    cJSON_GetObjectItemCaseSensitive(item, "when_context_contains"),
                                    "cron_jobs[].when_context_contains", 0);

      const cJSON *pre_wake = cJSON_GetObjectItemCaseSensitive(item, "pre_wake_gate");
      if (pre_wake)
      {
         if (!cJSON_IsBool(pre_wake))
            issues += config_issue("\"cron_jobs[%d].pre_wake_gate\" expected boolean, got %s",
                                   count, jo_type_name(pre_wake));
         else
            job->pre_wake_gate = cJSON_IsTrue(pre_wake) ? 1 : 0;
      }

      const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(item, "enabled");
      if (enabled)
      {
         if (!cJSON_IsBool(enabled))
            issues += config_issue("\"cron_jobs[%d].enabled\" expected boolean, got %s", count,
                                   jo_type_name(enabled));
         else
            job->enabled = cJSON_IsTrue(enabled) ? 1 : 0;
      }

      issues += config_parse_cron_job_skills(job, cJSON_GetObjectItemCaseSensitive(item, "skills"),
                                             count);
      issues += config_parse_cron_job_deliver(
          job, cJSON_GetObjectItemCaseSensitive(item, "deliver"), count);

      count++;
   }
   cfg->cron_job_count = count;
   return issues;
}

int config_parse_trigger(config_t *cfg, const cJSON *root)
{
   int issues = 0;

   /* Parse trigger.* settings */
   const cJSON *trigger = cJSON_GetObjectItemCaseSensitive(root, "trigger");
   if (trigger)
   {
      if (!cJSON_IsObject(trigger))
         return config_issue("\"trigger\" expected object, got %s", jo_type_name(trigger));

      const cJSON *auth_token = cJSON_GetObjectItemCaseSensitive(trigger, "auth_token");
      if (auth_token && cJSON_IsString(auth_token) && auth_token->valuestring[0])
         snprintf(cfg->trigger_auth_token, sizeof(cfg->trigger_auth_token), "%s",
                  auth_token->valuestring);

      const cJSON *max_concurrent = cJSON_GetObjectItemCaseSensitive(trigger, "max_concurrent");
      if (max_concurrent && cJSON_IsNumber(max_concurrent) && max_concurrent->valueint > 0)
         cfg->trigger_max_concurrent = max_concurrent->valueint;
   }

   /* Parse trigger_rules list (top-level, not nested under trigger) */
   const cJSON *rules = cJSON_GetObjectItemCaseSensitive(root, "trigger_rules");
   if (!rules)
      return config_parse_cron_jobs(cfg, root);
   if (!cJSON_IsArray(rules))
      return config_issue("\"trigger_rules\" expected array, got %s", jo_type_name(rules)) +
             config_parse_cron_jobs(cfg, root);

   int count = 0;
   const cJSON *rule;
   cJSON_ArrayForEach(rule, rules)
   {
      if (count >= TRIGGER_RULES_MAX)
      {
         issues += config_issue("too many trigger_rules (max %d)", TRIGGER_RULES_MAX);
         break;
      }
      if (!cJSON_IsObject(rule))
         continue;

      trigger_rule_t *r = &cfg->trigger_rules[count];

      const cJSON *source = cJSON_GetObjectItemCaseSensitive(rule, "source");
      if (source && cJSON_IsString(source) && source->valuestring[0])
         snprintf(r->source, sizeof(r->source), "%s", source->valuestring);

      const cJSON *event = cJSON_GetObjectItemCaseSensitive(rule, "event");
      if (event && cJSON_IsString(event) && event->valuestring[0])
         snprintf(r->event, sizeof(r->event), "%s", event->valuestring);

      const cJSON *schedule = cJSON_GetObjectItemCaseSensitive(rule, "schedule");
      if (schedule && cJSON_IsString(schedule) && schedule->valuestring[0])
         snprintf(r->schedule, sizeof(r->schedule), "%s", schedule->valuestring);

      const cJSON *mode = cJSON_GetObjectItemCaseSensitive(rule, "mode");
      if (mode && cJSON_IsString(mode) && mode->valuestring[0])
      {
         if (strcmp(mode->valuestring, "autonomous") != 0 &&
             strcmp(mode->valuestring, "interactive") != 0)
            issues += config_issue(
                "trigger_rules[].mode expected \"autonomous\" or \"interactive\", got \"%s\"",
                mode->valuestring);
         else
            snprintf(r->mode, sizeof(r->mode), "%s", mode->valuestring);
      }

      const cJSON *pipeline = cJSON_GetObjectItemCaseSensitive(rule, "pipeline");
      if (pipeline && cJSON_IsObject(pipeline))
      {
         const cJSON *tmpl = cJSON_GetObjectItemCaseSensitive(pipeline, "template");
         if (tmpl && cJSON_IsString(tmpl) && tmpl->valuestring[0])
            snprintf(r->pipeline_template, sizeof(r->pipeline_template), "%s", tmpl->valuestring);

         const cJSON *workspace = cJSON_GetObjectItemCaseSensitive(pipeline, "workspace");
         if (workspace && cJSON_IsString(workspace) && workspace->valuestring[0])
            snprintf(r->workspace, sizeof(r->workspace), "%s", workspace->valuestring);

         const cJSON *max_spend = cJSON_GetObjectItemCaseSensitive(pipeline, "max_spend_usd");
         if (max_spend && cJSON_IsNumber(max_spend))
            r->max_spend_usd = max_spend->valuedouble;
      }

      count++;
   }
   cfg->trigger_rule_count = count;
   return issues + config_parse_cron_jobs(cfg, root);
}

/* Inverse of config_parse_trigger + config_parse_cron_jobs: serialize the
 * trigger.* object, the trigger_rules[] list, and the cron_jobs[] list so
 * config_save (fresh tree) does not drop scheduled jobs / trigger rules on the
 * next save. Mirrors the parser key paths exactly (pipeline.* and deliver.* are
 * nested in YAML though flat in the struct). */
void config_save_trigger(const config_t *cfg, cJSON *root)
{
   if (!cfg || !root)
      return;

   /* trigger.auth_token is Vault-only; only non-secret settings are emitted. */
   if (cfg->trigger_max_concurrent != 2)
   {
      cJSON *t = cJSON_AddObjectToObject(root, "trigger");
      if (t)
      {
         cJSON_AddNumberToObject(t, "max_concurrent", cfg->trigger_max_concurrent);
      }
   }

   /* trigger_rules[] — {source,event,schedule,mode,pipeline:{template,workspace,max_spend_usd}} */
   if (cfg->trigger_rule_count > 0)
   {
      cJSON *rules = cJSON_AddArrayToObject(root, "trigger_rules");
      for (int i = 0; rules && i < cfg->trigger_rule_count && i < TRIGGER_RULES_MAX; i++)
      {
         const trigger_rule_t *r = &cfg->trigger_rules[i];
         cJSON *o = cJSON_CreateObject();
         if (!o)
            continue;
         if (r->source[0])
            cJSON_AddStringToObject(o, "source", r->source);
         if (r->event[0])
            cJSON_AddStringToObject(o, "event", r->event);
         if (r->schedule[0])
            cJSON_AddStringToObject(o, "schedule", r->schedule);
         if (r->mode[0])
            cJSON_AddStringToObject(o, "mode", r->mode);
         if (r->pipeline_template[0] || r->workspace[0] || r->max_spend_usd != 0.0)
         {
            cJSON *p = cJSON_AddObjectToObject(o, "pipeline");
            if (p)
            {
               if (r->pipeline_template[0])
                  cJSON_AddStringToObject(p, "template", r->pipeline_template);
               if (r->workspace[0])
                  cJSON_AddStringToObject(p, "workspace", r->workspace);
               if (r->max_spend_usd != 0.0)
                  cJSON_AddNumberToObject(p, "max_spend_usd", r->max_spend_usd);
            }
         }
         cJSON_AddItemToArray(rules, o);
      }
   }

   /* cron_jobs[] — typed scheduled jobs (skills[] + deliver:{} nested) */
   if (cfg->cron_job_count > 0)
   {
      cJSON *jobs = cJSON_AddArrayToObject(root, "cron_jobs");
      for (int i = 0; jobs && i < cfg->cron_job_count && i < CRON_JOBS_MAX; i++)
      {
         const cron_job_t *j = &cfg->cron_jobs[i];
         cJSON *o = cJSON_CreateObject();
         if (!o)
            continue;
         if (j->id[0])
            cJSON_AddStringToObject(o, "id", j->id);
         if (j->schedule[0])
            cJSON_AddStringToObject(o, "schedule", j->schedule);
         if (j->mode[0])
            cJSON_AddStringToObject(o, "mode", j->mode);
         if (j->script[0])
            cJSON_AddStringToObject(o, "script", j->script);
         if (j->prompt[0])
            cJSON_AddStringToObject(o, "prompt", j->prompt);
         if (j->workdir[0])
            cJSON_AddStringToObject(o, "workdir", j->workdir);
         if (j->context_from[0])
            cJSON_AddStringToObject(o, "context_from", j->context_from);
         if (j->when_context_contains[0])
            cJSON_AddStringToObject(o, "when_context_contains", j->when_context_contains);
         cJSON_AddBoolToObject(o, "pre_wake_gate", j->pre_wake_gate ? 1 : 0);
         cJSON_AddBoolToObject(o, "enabled", j->enabled ? 1 : 0);
         if (j->skill_count > 0)
         {
            cJSON *sk = cJSON_AddArrayToObject(o, "skills");
            for (int s = 0; sk && s < j->skill_count && s < CRON_JOB_MAX_SKILLS; s++)
               cJSON_AddItemToArray(sk, cJSON_CreateString(j->skills[s]));
         }
         if (j->deliver_target[0] || j->deliver_only_if_changed || j->deliver_first_run_silent)
         {
            cJSON *d = cJSON_AddObjectToObject(o, "deliver");
            if (d)
            {
               if (j->deliver_target[0])
                  cJSON_AddStringToObject(d, "target", j->deliver_target);
               cJSON_AddBoolToObject(d, "only_if_changed", j->deliver_only_if_changed ? 1 : 0);
               cJSON_AddBoolToObject(d, "first_run_silent", j->deliver_first_run_silent ? 1 : 0);
            }
         }
         cJSON_AddItemToArray(jobs, o);
      }
   }
}
