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
                                 "  \"aggregator\": \"ignored-legacy-value\","
                                 "  \"chairman\": \"codex\","
                                 "  \"chairman_enabled\": true,"
                                 "  \"min_successful\": 2,"
                                 "  \"max_cost_usd\": 1.5,"
                                 "  \"max_rounds\": 3,"
                                 "  \"converge_threshold\": 2,"
                                 "  \"deadline_ms\": 600000,"
                                 "  \"discussion\": true,"
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
   assert(strcmp(p->chairman, "codex") == 0);
   assert(p->chairman_enabled == 1);
   assert(p->min_successful == 2);
   assert(p->max_cost_usd == 1.5);
   assert(p->max_rounds == 3);
   assert(p->converge_threshold == 2);
   assert(p->deadline_ms == 600000);
   assert(p->discussion == 1);
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

   roundtable_preset_t invalid;
   const char *errmsg = NULL;
   assert(roundtable_preset_from_json("{\"chairman_enabled\":true}", "invalid", &invalid,
                                      &errmsg) != 0);
   assert(errmsg && strcmp(errmsg, "chairman_enabled requires a chairman") == 0);

   /* A PANEL OF ONE HAS NOBODY TO CHAIR, SYNTHESISE OR DISCUSS WITH.
    *
    * Each of these arbitrates or exchanges BETWEEN seats; with one seat the
    * agent would be discussing with itself. Measured: a one-seat review returned
    * a correct blocking finding and the chair then died on "unknown persona
    * 'chairman'", failing the whole run and hiding the finding. Refused at
    * intake so the operator is told, rather than having it silently dropped. */
   {
      roundtable_preset_t one;
      const char *e1 = NULL;
      assert(roundtable_preset_from_json("{\"seats\":[{\"model\":\"m\"}],\"chairman\":\"c\"}",
                                         "one-chair", &one, &e1) != 0);
      assert(e1 && strstr(e1, "roundtable of one") != NULL);

      const char *e2 = NULL;
      assert(roundtable_preset_from_json("{\"seats\":[{\"model\":\"m\"}],\"discussion\":true}",
                                         "one-disc", &one, &e2) != 0);
      assert(e2 && strstr(e2, "roundtable of one") != NULL);

      /* Two seats may have all three. */
      const char *e3 = NULL;
      assert(roundtable_preset_from_json(
                 "{\"seats\":[{\"model\":\"a\"},{\"model\":\"b\"}],"
                 "\"chairman\":\"c\",\"chairman_enabled\":true,\"discussion\":true}",
                 "two-ok", &one, &e3) == 0);
   }

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
   assert(cJSON_GetObjectItemCaseSensitive(j, "aggregator") == NULL);
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

   /* Runtime resolution uses an explicit preset exactly, otherwise the saved
    * default. It never invents or expands seats. */
   ensemble_panel_t runtime;
   memset(&runtime, 0, sizeof runtime);
   char resolved[RT_PRESET_NAME_MAX], rerr[128];
   assert(roundtable_preset_resolve_runtime("deep-review", &runtime, resolved, sizeof resolved,
                                            rerr, sizeof rerr) == 1);
   assert(strcmp(resolved, "deep-review") == 0);
   assert(runtime.reference_count == 3);
   assert(strcmp(runtime.reference_models[2], "glm") == 0);
   assert(roundtable_preset_resolve_runtime("missing", &runtime, resolved, sizeof resolved, rerr,
                                            sizeof rerr) == -1);

   roundtable_preset_t default_preset = p;
   snprintf(default_preset.name, sizeof default_preset.name, "default");
   assert(roundtable_preset_save(&default_preset) == 0);
   memset(&runtime, 0, sizeof runtime);
   assert(roundtable_preset_resolve_runtime(NULL, &runtime, resolved, sizeof resolved, rerr,
                                            sizeof rerr) == 1);
   assert(strcmp(resolved, "default") == 0);
   assert(runtime.reference_count == 3);

   /* apply_to_config mirrors the preset onto the live config_t */
   assert(config_set_ensemble_aggregator("c-only") == 0);
   char aerr[128];
   assert(roundtable_preset_apply_to_config("deep-review", aerr, sizeof(aerr)) == 0);
   assert(config_ensemble_reference_count() == 3);
   assert(config_ensemble_reference_persona_count() == 3);
   assert(strcmp(config_ensemble_reference_models(0), "codex") == 0);
   assert(strcmp(config_ensemble_reference_personas(0), "reviewer") == 0);
   assert(strcmp(config_ensemble_reference_models(1), "gpu-mid") == 0);
   assert(strcmp(config_ensemble_reference_personas(1), "security") == 0);
   /* Applying a Go roundtable preset must not silently mutate the separate C
    * compatibility route while that route still exists. */
   assert(strcmp(config_ensemble_aggregator(), "c-only") == 0);
   assert(config_ensemble_min_successful() == 2);
   assert(config_roundtable_max_rounds() == 3);
   assert(config_roundtable_converge_threshold() == 2);
   assert(config_roundtable_deadline_ms() == 600000);
   assert(strcmp(config_roundtable_turns(), "parallel") == 0);
   assert(config_roundtable_pipeline_gate_ttl_h() == 24);
   assert(strcmp(config_roundtable_default(), "deep-review") == 0);

   /* delete removes the file */
   assert(roundtable_preset_delete("deep-review") == 0);
   assert(roundtable_preset_delete("default") == 0);
   assert(roundtable_preset_load("deep-review", &loaded) != 0);
   assert(roundtable_preset_delete("deep-review") != 0); /* already gone */

   printf("ok\n");
   return 0;
}
