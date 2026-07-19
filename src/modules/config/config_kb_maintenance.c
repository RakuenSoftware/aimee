/* config_kb_maintenance.c: parser for the `kb.maintenance` config section.
 * Split out of config.c to keep that file under the line-count gate.
 *
 * kb:
 *   maintenance:
 *     enabled: false
 *     interval_hours: 24
 *     lambda: 0.005
 *     floor: 0.10
 *     min_age_days: 7
 *     orphan_prune_days: 90
 */

#include "aimee.h"
#include "config.h"
#include "json_fluent.h"
#include "cJSON.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int config_parse_kb_maintenance(config_t *cfg, const cJSON *root)
{
   const cJSON *kb = cJSON_GetObjectItemCaseSensitive(root, "kb");
   if (!kb)
      return 0;
   if (!cJSON_IsObject(kb))
      return config_issue("\"kb\" expected object, got %s", jo_type_name(kb));

   const cJSON *maintenance = cJSON_GetObjectItemCaseSensitive(kb, "maintenance");
   if (!maintenance)
      return 0;
   if (!cJSON_IsObject(maintenance))
      return config_issue("\"kb.maintenance\" expected object, got %s", jo_type_name(maintenance));

   const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(maintenance, "enabled");
   if (enabled && cJSON_IsBool(enabled))
      cfg->kb_maintenance_enabled = cJSON_IsTrue(enabled) ? 1 : 0;

   const cJSON *interval_hours = cJSON_GetObjectItemCaseSensitive(maintenance, "interval_hours");
   if (interval_hours && cJSON_IsNumber(interval_hours) && interval_hours->valueint > 0)
      cfg->kb_maintenance_interval_hours = interval_hours->valueint;

   const cJSON *lambda = cJSON_GetObjectItemCaseSensitive(maintenance, "lambda");
   if (lambda && cJSON_IsNumber(lambda) && lambda->valuedouble > 0.0)
      cfg->kb_maintenance_lambda = lambda->valuedouble;

   const cJSON *floor = cJSON_GetObjectItemCaseSensitive(maintenance, "floor");
   if (floor && cJSON_IsNumber(floor) && floor->valuedouble >= 0.0 && floor->valuedouble <= 1.0)
      cfg->kb_maintenance_floor = floor->valuedouble;

   const cJSON *min_age_days = cJSON_GetObjectItemCaseSensitive(maintenance, "min_age_days");
   if (min_age_days && cJSON_IsNumber(min_age_days) && min_age_days->valueint >= 0)
      cfg->kb_maintenance_min_age_days = min_age_days->valueint;

   const cJSON *orphan_prune_days =
       cJSON_GetObjectItemCaseSensitive(maintenance, "orphan_prune_days");
   if (orphan_prune_days && cJSON_IsNumber(orphan_prune_days) && orphan_prune_days->valueint >= 0)
      cfg->kb_maintenance_orphan_days = orphan_prune_days->valueint;

   return 0;
}

/* Inverse of config_parse_kb_maintenance: serialize kb.maintenance.* into the
 * shared root so config_save (which builds a fresh tree) does not silently drop a
 * hand-set or `aimee`-set maintenance block on the next save. Only emitted when
 * something differs from the defaults. Mirrors config_save_kb_curator. */
void config_save_kb_maintenance(const config_t *cfg, cJSON *root)
{
   if (!cfg || !root)
      return;
   int any = cfg->kb_maintenance_enabled || cfg->kb_maintenance_interval_hours != 24 ||
             cfg->kb_maintenance_lambda != 0.005 || cfg->kb_maintenance_floor != 0.10 ||
             cfg->kb_maintenance_min_age_days != 7 || cfg->kb_maintenance_orphan_days != 90;
   if (!any)
      return;

   cJSON *kb = cJSON_GetObjectItemCaseSensitive(root, "kb");
   if (!kb)
      kb = cJSON_AddObjectToObject(root, "kb");
   if (!kb)
      return;
   cJSON *m = cJSON_AddObjectToObject(kb, "maintenance");
   if (!m)
      return;
   cJSON_AddBoolToObject(m, "enabled", cfg->kb_maintenance_enabled ? 1 : 0);
   cJSON_AddNumberToObject(m, "interval_hours", cfg->kb_maintenance_interval_hours);
   cJSON_AddNumberToObject(m, "lambda", cfg->kb_maintenance_lambda);
   cJSON_AddNumberToObject(m, "floor", cfg->kb_maintenance_floor);
   cJSON_AddNumberToObject(m, "min_age_days", cfg->kb_maintenance_min_age_days);
   cJSON_AddNumberToObject(m, "orphan_prune_days", cfg->kb_maintenance_orphan_days);
}
