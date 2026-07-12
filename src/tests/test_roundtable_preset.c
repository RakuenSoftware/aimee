/* test_roundtable_preset.c: round-trip a named roundtable preset through
 * from_json -> save -> load and to_json, and verify apply_to_config mirrors the
 * preset onto the live config_t ensemble/roundtable fields. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "config.h"
#include "roundtable_preset.h"
#include "platform_path.h"
#include "platform_test_util.h"

static const char *PRESET_JSON = "{"
                                 "  \"name\": \"deep-review\","
                                 "  \"description\": \"thorough multi-model review\","
                                 "  \"seats\": ["
                                 "    { \"model\": \"codex\", \"persona\": \"reviewer\" },"
                                 "    { \"model\": \"gpu-mid\", \"persona\": \"security\" },"
                                 "    { \"model\": \"glm\", \"persona\": \"\" }"
                                 "  ],"
                                 "  \"aggregator\": \"claude\","
                                 "  \"min_successful\": 2,"
                                 "  \"max_cost_usd\": 1.5,"
                                 "  \"max_rounds\": 3,"
                                 "  \"converge_threshold\": 2,"
                                 "  \"deadline_ms\": 360000,"
                                 "  \"turns\": \"parallel\","
                                 "  \"pipeline\": {"
                                 "    \"done_bar\": \"zero_blocking\","
                                 "    \"max_passes\": 4,"
                                 "    \"max_attempts_per_pass\": 3,"
                                 "    \"max_cost_usd\": 2.0,"
                                 "    \"max_total_cost_usd\": 8.0,"
                                 "    \"gate_ttl_h\": 24,"
                                 "    \"parked_releases_slot\": 0,"
                                 "    \"unknown_context_tokens\": 12000"
                                 "  }"
                                 "}";

static void check_fields(const roundtable_preset_t *p)
{
   assert(strcmp(p->name, "deep-review") == 0);
   assert(strcmp(p->description, "thorough multi-model review") == 0);
   assert(p->seat_count == 3);
   assert(strcmp(p->seats[0].model, "codex") == 0);
   assert(strcmp(p->seats[0].persona, "reviewer") == 0);
   assert(strcmp(p->seats[1].model, "gpu-mid") == 0);
   assert(strcmp(p->seats[1].persona, "security") == 0);
   assert(strcmp(p->seats[2].model, "glm") == 0);
   assert(p->seats[2].persona[0] == '\0');
   assert(strcmp(p->aggregator, "claude") == 0);
   assert(p->min_successful == 2);
   assert(p->max_cost_usd == 1.5);
   assert(p->max_rounds == 3);
   assert(p->converge_threshold == 2);
   assert(p->deadline_ms == 360000);
   assert(strcmp(p->turns, "parallel") == 0);
   assert(strcmp(p->pipeline_done_bar, "zero_blocking") == 0);
   assert(p->pipeline_max_passes == 4);
   assert(p->pipeline_max_attempts_per_pass == 3);
   assert(p->pipeline_max_cost_usd == 2.0);
   assert(p->pipeline_max_total_cost_usd == 8.0);
   assert(p->pipeline_gate_ttl_h == 24);
   assert(p->pipeline_parked_releases_slot == 0);
   assert(p->pipeline_unknown_context_tokens == 12000);
}

int main(void)
{
   printf("roundtable_preset: ");

   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-rtpreset-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   platform_setenv("HOME", tmpdir);
   platform_unsetenv("AIMEE_HOME");
   platform_setenv("AIMEE_NO_CACHE", "1");

   /* name validation */
   assert(roundtable_preset_name_valid("deep-review"));
   assert(roundtable_preset_name_valid("default"));
   assert(!roundtable_preset_name_valid(""));
   assert(!roundtable_preset_name_valid(".hidden"));
   assert(!roundtable_preset_name_valid("has space"));
   assert(!roundtable_preset_name_valid("slash/name"));

   /* parse -> fields */
   roundtable_preset_t p;
   const char *err = NULL;
   assert(roundtable_preset_from_json(PRESET_JSON, NULL, &p, &err) == 0);
   check_fields(&p);

   /* url_name overrides body name */
   roundtable_preset_t p2;
   assert(roundtable_preset_from_json(PRESET_JSON, "override-name", &p2, &err) == 0);
   assert(strcmp(p2.name, "override-name") == 0);

   /* save -> load round-trip preserves every field */
   assert(roundtable_preset_save(&p) == 0);
   roundtable_preset_t loaded;
   assert(roundtable_preset_load("deep-review", &loaded) == 0);
   check_fields(&loaded);

   /* to_json emits a re-parseable object */
   cJSON *j = roundtable_preset_to_json(&loaded);
   assert(j != NULL);
   char *text = cJSON_PrintUnformatted(j);
   cJSON_Delete(j);
   assert(text != NULL);
   roundtable_preset_t reparsed;
   assert(roundtable_preset_from_json(text, NULL, &reparsed, &err) == 0);
   free(text);
   check_fields(&reparsed);

   /* list finds the saved preset */
   char names[16][RT_PRESET_NAME_MAX];
   int n = roundtable_preset_list(names, 16);
   assert(n == 1);
   assert(strcmp(names[0], "deep-review") == 0);

   /* load of a missing preset fails cleanly */
   roundtable_preset_t missing;
   assert(roundtable_preset_load("nope", &missing) != 0);

   /* apply_to_config mirrors the preset onto the live config_t */
   char aerr[128];
   assert(roundtable_preset_apply_to_config("deep-review", aerr, sizeof(aerr)) == 0);
   config_t cfg;
   assert(config_load(&cfg) == 0);
   assert(cfg.ensemble_reference_count == 3);
   assert(cfg.ensemble_reference_persona_count == 3);
   assert(strcmp(cfg.ensemble_reference_models[0], "codex") == 0);
   assert(strcmp(cfg.ensemble_reference_personas[0], "reviewer") == 0);
   assert(strcmp(cfg.ensemble_reference_models[1], "gpu-mid") == 0);
   assert(strcmp(cfg.ensemble_reference_personas[1], "security") == 0);
   assert(strcmp(cfg.ensemble_aggregator, "claude") == 0);
   assert(cfg.ensemble_min_successful == 2);
   assert(cfg.roundtable_max_rounds == 3);
   assert(cfg.roundtable_converge_threshold == 2);
   assert(cfg.roundtable_deadline_ms == 360000);
   assert(strcmp(cfg.roundtable_turns, "parallel") == 0);
   assert(cfg.roundtable_pipeline_gate_ttl_h == 24);
   assert(strcmp(cfg.roundtable_default, "deep-review") == 0);

   /* delete removes the file */
   assert(roundtable_preset_delete("deep-review") == 0);
   assert(roundtable_preset_load("deep-review", &loaded) != 0);
   assert(roundtable_preset_delete("deep-review") != 0); /* already gone */

   printf("ok\n");
   return 0;
}
