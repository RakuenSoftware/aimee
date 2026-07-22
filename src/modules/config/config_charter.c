/* config_charter.c: parser for the top-level `charter` YAML section.
 * Split out of config.c to keep that file under the line-count gate.
 * The charter is operator-authored and immutable at runtime — no
 * setter exists; editing aimee.yaml is the only path. */

#include "aimee.h"
#include "config.h"
#include "json_fluent.h"
#include "cJSON.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Surface a warning to stderr, same shape as config.c's config_issue. */
/* Parse a YAML array under charter.<name> into a fixed-size buffer.
 * Non-string and empty entries are skipped so a typo doesn't poison
 * the whole load. Returns the number of entries populated. */
static int charter_parse_array(const cJSON *arr, const char *path,
                               char out[][CONFIG_CHARTER_ENTRY_LEN], int *count_out)
{
   if (!arr)
      return 0;
   if (!cJSON_IsArray(arr))
   {
      config_issue("\"%s\" expected array, got %s", path, jo_type_name(arr));
      return 0;
   }
   int count = 0;
   const cJSON *entry;
   cJSON_ArrayForEach(entry, arr)
   {
      if (count >= CONFIG_CHARTER_MAX_ENTRIES)
      {
         config_issue("too many entries in \"%s\" (max %d)", path, CONFIG_CHARTER_MAX_ENTRIES);
         break;
      }
      if (!cJSON_IsString(entry) || !entry->valuestring[0])
         continue;
      snprintf(out[count], CONFIG_CHARTER_ENTRY_LEN, "%s", entry->valuestring);
      count++;
   }
   if (count_out)
      *count_out = count;
   return count;
}

int config_parse_charter(config_t *cfg, const cJSON *root)
{
   const cJSON *charter = cJSON_GetObjectItemCaseSensitive(root, "charter");
   if (!charter)
      return 0;
   if (!cJSON_IsObject(charter))
      return config_issue("\"charter\" expected object, got %s", jo_type_name(charter));

   charter_parse_array(cJSON_GetObjectItemCaseSensitive(charter, "safety_axioms"),
                       "charter.safety_axioms", cfg->charter_safety_axioms,
                       &cfg->charter_safety_axioms_count);
   charter_parse_array(cJSON_GetObjectItemCaseSensitive(charter, "hard_constraints"),
                       "charter.hard_constraints", cfg->charter_hard_constraints,
                       &cfg->charter_hard_constraints_count);
   charter_parse_array(cJSON_GetObjectItemCaseSensitive(charter, "values"), "charter.values",
                       cfg->charter_values, &cfg->charter_values_count);
   charter_parse_array(cJSON_GetObjectItemCaseSensitive(charter, "tone_boundaries"),
                       "charter.tone_boundaries", cfg->charter_tone_boundaries,
                       &cfg->charter_tone_boundaries_count);

   const cJSON *drift = cJSON_GetObjectItemCaseSensitive(charter, "working_profile_drift_limit");
   if (drift && cJSON_IsNumber(drift) && drift->valueint >= 0)
      cfg->charter_working_profile_drift_limit = drift->valueint;
   return 0;
}

/* Inverse of config_parse_charter: serialize charter.* into the shared root so
 * config_save (which builds a fresh tree) does not drop the operator-authored
 * charter on the next save. Mirrors config_save_kb_curator. */
static void charter_emit_array(cJSON *parent, const char *key,
                               const char arr[][CONFIG_CHARTER_ENTRY_LEN], int count)
{
   if (count <= 0)
      return;
   cJSON *a = cJSON_AddArrayToObject(parent, key);
   if (!a)
      return;
   for (int i = 0; i < count && i < CONFIG_CHARTER_MAX_ENTRIES; i++)
      cJSON_AddItemToArray(a, cJSON_CreateString(arr[i]));
}

void config_save_charter(const config_t *cfg, cJSON *root)
{
   if (!cfg || !root)
      return;
   if (!cfg->charter_safety_axioms_count && !cfg->charter_hard_constraints_count &&
       !cfg->charter_values_count && !cfg->charter_tone_boundaries_count &&
       cfg->charter_working_profile_drift_limit == 0)
      return;
   cJSON *charter = cJSON_AddObjectToObject(root, "charter");
   if (!charter)
      return;
   charter_emit_array(charter, "safety_axioms", cfg->charter_safety_axioms,
                      cfg->charter_safety_axioms_count);
   charter_emit_array(charter, "hard_constraints", cfg->charter_hard_constraints,
                      cfg->charter_hard_constraints_count);
   charter_emit_array(charter, "values", cfg->charter_values, cfg->charter_values_count);
   charter_emit_array(charter, "tone_boundaries", cfg->charter_tone_boundaries,
                      cfg->charter_tone_boundaries_count);
   if (cfg->charter_working_profile_drift_limit != 0)
      cJSON_AddNumberToObject(charter, "working_profile_drift_limit",
                              cfg->charter_working_profile_drift_limit);
}
