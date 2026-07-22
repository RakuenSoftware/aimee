/* test_config_economizer.c: the proof-gated economizer control
 * (economizer: {mode: off|proof_gated}) — mode resolution, defaults, and the
 * save/load round-trip — plus the Coordinate Closet tuning round-trip. Kept
 * separate from test_config.c, which is at the build-integrity line-count limit. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aimee.h"
#include "config_sections.h"
#include "config_fields.h"
#include "aimee_home.h"
#include "platform_path.h"
#include "platform_test_util.h"

/* Mode resolution: neither public mode enables the disconnected legacy mutation
 * levers. modules.economizer:false is an authoritative hard kill. Pure — no I/O. */
static void test_tier_resolution(void)
{
   config_t c;
   memset(&c, 0, sizeof c);
   c.module_economizer = -1; /* unspecified -> mode is authoritative */

   econ_preset_t ep;

   /* OFF: no economization at all -> every legacy preset lever is off. */
   c.economizer_mode = ECON_MODE_OFF;
   assert(econ_mode(&c) == ECON_MODE_OFF);
   assert(econ_reduction_master_on(&c) == 0);
   assert(econ_gateway_mutate_on(&c) == 0);
   econ_preset(&c, &ep);
   assert(ep.history_fold == 0 && ep.command_filter == 0 && ep.compress == 0 &&
          ep.gateway_seam == 0);

   /* PROOF_GATED authorizes only the new proof path. Old lossy levers remain dead. */
   c.economizer_mode = ECON_MODE_PROOF_GATED;
   assert(econ_mode(&c) == ECON_MODE_PROOF_GATED);
   assert(econ_reduction_master_on(&c) == 0);
   assert(econ_gateway_mutate_on(&c) == 0);
   econ_preset(&c, &ep);
   assert(ep.history_fold == 0 && ep.command_filter == 0 && ep.compress == 0 &&
          ep.gateway_seam == 0);

   /* modules.economizer:false is an authoritative hard-kill over either mode. */
   c.module_economizer = 0;
   assert(econ_mode(&c) == ECON_MODE_OFF);
   assert(econ_reduction_master_on(&c) == 0);
   assert(econ_gateway_mutate_on(&c) == 0);
   econ_preset(&c, &ep);
   assert(ep.history_fold == 0 && ep.gateway_seam == 0);

   /* The parser is exact and rejects every legacy/unknown spelling. */
   assert(econ_mode_parse("off") == ECON_MODE_OFF);
   assert(econ_mode_parse("proof_gated") == ECON_MODE_PROOF_GATED);
   assert(econ_mode_parse("safe") == -1);
   assert(econ_mode_parse("aggressive") == -1);
   assert(econ_mode_parse("aggro") == -1);
   assert(econ_mode_parse("bogus") == -1);
   assert(econ_mode_parse("OFF") == -1);
   assert(econ_mode_parse(NULL) == -1);
   assert(strcmp(econ_mode_name(ECON_MODE_OFF), "off") == 0);
   assert(strcmp(econ_mode_name(ECON_MODE_PROOF_GATED), "proof_gated") == 0);

   /* NULL-safe */
   assert(econ_reduction_master_on(NULL) == 0);
   assert(econ_gateway_mutate_on(NULL) == 0);
   econ_preset(NULL, &ep); /* NULL cfg -> OFF -> all zero */
   assert(ep.history_fold == 0 && ep.command_filter == 0);
}

/* live-config-reload: reload-class classification + verdict for representative
 * HOT (snapshot-live) and RESTART (startup-only) fields. */
static void test_reload_class(void)
{
   const config_field_t *rst = config_field_lookup("db2_url");           /* startup: pg pool */
   const config_field_t *aut = config_field_lookup("autonomy.skeptics"); /* live via snapshot */
   assert(rst && rst->reload_class == RELOAD_RESTART);
   assert(aut && aut->reload_class == RELOAD_HOT);
   assert(strstr(config_field_reload_verdict(rst), "restart"));
   assert(strcmp(config_field_reload_verdict(aut), "applied live") == 0);

   /* the retired per-lever economizer keys are no longer a typed config surface. */
   assert(config_field_lookup("economizer.aggressive") == NULL);
   assert(config_field_lookup("economizer.enabled") == NULL);
   assert(config_field_lookup("reduce.history_fold") == NULL);
   assert(config_field_lookup("reduce.command_filter") == NULL);

   /* The single economizer.mode field is HOT and accepts only public modes. */
   const config_field_t *econ = config_field_lookup("economizer.mode");
   assert(econ && econ->reload_class == RELOAD_HOT);
   config_t c;
   memset(&c, 0, sizeof c);
   c.economizer_mode = ECON_MODE_OFF;
   assert(config_field_set_value(&c, econ, "proof_gated") == 0 &&
          c.economizer_mode == ECON_MODE_PROOF_GATED);
   assert(config_field_set_value(&c, econ, "off") == 0 && c.economizer_mode == ECON_MODE_OFF);
   assert(config_field_set_value(&c, econ, "safe") == -1);
   assert(config_field_set_value(&c, econ, "aggressive") == -1);
   assert(config_field_set_value(&c, econ, "bogus") == -1); /* invalid token rejected */
   cJSON *v = config_field_value_json(&c, econ);            /* reads back as a string */
   assert(cJSON_IsString(v) && strcmp(v->valuestring, "off") == 0);
   cJSON_Delete(v);
}

int main(void)
{
   printf("config_economizer: ");
   test_tier_resolution();
   test_reload_class();

   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-econ-cfg-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   char *old_aimee_home = getenv("AIMEE_HOME") ? strdup(getenv("AIMEE_HOME")) : NULL;
   char *old_no_cache = getenv("AIMEE_NO_CACHE") ? strdup(getenv("AIMEE_NO_CACHE")) : NULL;
   platform_setenv("HOME", tmpdir);
   platform_unsetenv("AIMEE_HOME");
   platform_setenv("AIMEE_NO_CACHE", "1");

   /* --- defaults: economizer OFF, Coordinate Closet on --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg); /* missing file -> defaults */
      assert(cfg.economizer_mode == ECON_MODE_OFF);
      assert(cfg.coord_closet_enabled == 1);
   }

   /* --- economizer mode: save/load round-trip (nested object form) --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      assert(cfg.economizer_mode == ECON_MODE_OFF);
      cfg.economizer_mode = ECON_MODE_PROOF_GATED;
      assert(config_save(&cfg) == 0);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(cfg2.economizer_mode == ECON_MODE_PROOF_GATED);

      /* OFF is represented by omission and reloads to the fail-closed default. */
      cfg.economizer_mode = ECON_MODE_OFF;
      assert(config_save(&cfg) == 0);
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(cfg2.economizer_mode == ECON_MODE_OFF);
   }

   /* --- only the exact nested public shape is accepted; legacy forms hard fail --- */
   {
      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      cJSON *root = cJSON_Parse("{\"economizer\":{\"mode\":\"proof_gated\"}}");
      assert(root);
      assert(config_parse_economizer_section(&cfg, root) == 0);
      assert(cfg.economizer_mode == ECON_MODE_PROOF_GATED);
      cJSON_Delete(root);

      const char *invalid[] = {
          "{\"economizer\":\"safe\"}",
          "{\"economizer\":\"aggressive\"}",
          "{\"economizer\":{\"enabled\":false}}",
          "{\"economizer\":{\"mode\":\"safe\"}}",
          "{\"economizer\":{\"mode\":\"aggressive\"}}",
          "{\"economizer\":{\"mode\":\"off\",\"extra\":true}}",
          "{\"economizer\":{\"mode\":\"off\",\"mode\":\"proof_gated\"}}",
          "{\"economizer\":{\"mode\":true}}",
          "{\"economizer\":{}}",
      };
      for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++)
      {
         root = cJSON_Parse(invalid[i]);
         assert(root);
         assert(config_parse_economizer_section(&cfg, root) == -1);
         cJSON_Delete(root);
      }
   }

   /* --- Coordinate Closet tuning (compact.coord_closet) round-trips byte-equal --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      cfg.coord_closet_enabled = 1;
      cfg.coord_closet_budget_bytes = 4096;
      cfg.coord_closet_max_ratio_pct = 15;
      snprintf(cfg.coord_closet_denylist, sizeof(cfg.coord_closet_denylist), "secretword");
      assert(config_save(&cfg) == 0);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(cfg2.coord_closet_enabled == 1);
      assert(cfg2.coord_closet_budget_bytes == 4096);
      assert(cfg2.coord_closet_max_ratio_pct == 15);
      assert(strcmp(cfg2.coord_closet_denylist, "secretword") == 0);
   }

   /* --- modules.* tristate: defaults are -1 (unspecified) and are NOT persisted; an explicit
    * 0/1 round-trips; and the resolver maps -1 -> env default, 0/1 -> canonical. --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg); /* no modules: block -> all unspecified */
      assert(cfg.module_memory == -1 && cfg.module_governance == -1);
      assert(cfg.module_delegates == -1 && cfg.module_workflows == -1);
      assert(cfg.module_economizer == -1);

      /* save the pristine defaults: nothing written -> reload still -1 (no stray 0). */
      assert(config_save(&cfg) == 0);
      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(cfg2.module_memory == -1 && cfg2.module_governance == -1);
      assert(cfg2.module_delegates == -1 && cfg2.module_workflows == -1);
      assert(cfg2.module_economizer == -1);

      /* explicit user toggles round-trip: governance OFF, delegates ON, economizer OFF. */
      cfg2.module_governance = 0;
      cfg2.module_delegates = 1;
      cfg2.module_economizer = 0;
      assert(config_save(&cfg2) == 0);
      static config_t cfg3;
      memset(&cfg3, 0, sizeof(cfg3));
      config_load(&cfg3);
      assert(cfg3.module_governance == 0); /* explicit disable persisted */
      assert(cfg3.module_delegates == 1);  /* explicit enable persisted  */
      assert(cfg3.module_economizer == 0); /* explicit disable persisted */
      assert(cfg3.module_memory == -1 && cfg3.module_workflows == -1); /* untouched stay -1 */

      /* resolver precedence (governance is default-ON via env fallback when unspecified). */
      assert(config_module_enabled(cfg3.module_governance, 1) == 0); /* config OFF wins */
      assert(config_module_enabled(cfg3.module_memory, 1) == 1);     /* -1 -> env default ON */
      assert(config_module_enabled(cfg3.module_memory, 0) == 0);     /* -1 -> env default OFF */

      /* modules.economizer:false hard-kills proof_gated; legacy reducers stay disabled. */
      config_t ec;
      memset(&ec, 0, sizeof(ec));
      ec.economizer_mode = ECON_MODE_PROOF_GATED;
      ec.module_economizer = 0;
      assert(econ_mode(&ec) == ECON_MODE_OFF);
      assert(econ_reduction_master_on(&ec) == 0);
      ec.module_economizer = 1;
      assert(econ_mode(&ec) == ECON_MODE_PROOF_GATED);
      assert(econ_reduction_master_on(&ec) == 0);
      ec.module_economizer = -1;
      assert(econ_mode(&ec) == ECON_MODE_PROOF_GATED);
      assert(econ_reduction_master_on(&ec) == 0);
      ec.economizer_mode = ECON_MODE_OFF;
      assert(econ_reduction_master_on(&ec) == 0);
   }

   /* restore env */
   if (old_home)
   {
      platform_setenv("HOME", old_home);
      free(old_home);
   }
   if (old_aimee_home)
   {
      platform_setenv("AIMEE_HOME", old_aimee_home);
      free(old_aimee_home);
   }
   else
      platform_unsetenv("AIMEE_HOME");
   if (old_no_cache)
   {
      platform_setenv("AIMEE_NO_CACHE", old_no_cache);
      free(old_no_cache);
   }
   else
      platform_unsetenv("AIMEE_NO_CACHE");

   printf("ok\n");
   return 0;
}
