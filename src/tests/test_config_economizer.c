/* test_config_economizer.c: the single tiered economizer control
 * (economizer: off|safe|aggressive) — tier resolution, defaults, and the
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

/* Tier resolution: OFF disables all economization; SAFE enables reduction without
 * live gateway mutation; AGGRESSIVE adds live mutation; modules.economizer:false is
 * an authoritative hard-kill over any tier. Pure — no file I/O. */
static void test_tier_resolution(void)
{
   config_t c;
   memset(&c, 0, sizeof c);
   c.module_economizer = -1; /* unspecified -> tier is authoritative */

   econ_preset_t ep;

   /* OFF tier: no economization at all -> every preset lever is off. */
   c.economizer_tier = ECON_TIER_OFF;
   assert(econ_tier(&c) == ECON_TIER_OFF);
   assert(econ_reduction_master_on(&c) == 0);
   assert(econ_gateway_mutate_on(&c) == 0);
   econ_preset(&c, &ep);
   assert(ep.history_fold == 0 && ep.command_filter == 0 && ep.compress == 0 &&
          ep.gateway_seam == 0);

   /* SAFE tier: reduction on (fold + condensation), but no live gateway mutation. */
   c.economizer_tier = ECON_TIER_SAFE;
   assert(econ_reduction_master_on(&c) == 1);
   assert(econ_gateway_mutate_on(&c) == 0); /* live mutation is aggressive-only */
   econ_preset(&c, &ep);
   assert(ep.history_fold == 1 && ep.command_filter == 1);
   assert(ep.compress == 0 && ep.gateway_seam == 0); /* lossy/mutating levers off */
   assert(ep.freeze_guard_horizon == 1);

   /* AGGRESSIVE tier: reduction on + lossy compress + live mutation (OpenAI-only
    * enforced at the call site). */
   c.economizer_tier = ECON_TIER_AGGRESSIVE;
   assert(econ_reduction_master_on(&c) == 1);
   assert(econ_gateway_mutate_on(&c) == 1);
   econ_preset(&c, &ep);
   assert(ep.history_fold == 1 && ep.command_filter == 1);
   assert(ep.compress == 1 && ep.gateway_seam == 1);
   assert(ep.gateway_session_disable_ttl_ms == 3600000);

   /* modules.economizer:false is an authoritative hard-kill over any tier. */
   c.module_economizer = 0;
   assert(econ_tier(&c) == ECON_TIER_OFF);
   assert(econ_reduction_master_on(&c) == 0);
   assert(econ_gateway_mutate_on(&c) == 0);
   econ_preset(&c, &ep);
   assert(ep.history_fold == 0 && ep.gateway_seam == 0);

   /* the tier parser + names round-trip. */
   assert(econ_tier_parse("off") == ECON_TIER_OFF);
   assert(econ_tier_parse("safe") == ECON_TIER_SAFE);
   assert(econ_tier_parse("aggressive") == ECON_TIER_AGGRESSIVE);
   assert(econ_tier_parse("aggro") == ECON_TIER_AGGRESSIVE);
   assert(econ_tier_parse("bogus") == ECON_TIER_SAFE); /* unknown -> safe */
   assert(strcmp(econ_tier_name(ECON_TIER_OFF), "off") == 0);
   assert(strcmp(econ_tier_name(ECON_TIER_SAFE), "safe") == 0);
   assert(strcmp(econ_tier_name(ECON_TIER_AGGRESSIVE), "aggressive") == 0);

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

   /* the single `economizer` tier IS a settable HOT field (get/set as a string). */
   const config_field_t *econ = config_field_lookup("economizer");
   assert(econ && econ->reload_class == RELOAD_HOT);
   config_t c;
   memset(&c, 0, sizeof c);
   c.economizer_tier = ECON_TIER_SAFE;
   assert(config_field_set_value(&c, econ, "aggressive") == 0 &&
          c.economizer_tier == ECON_TIER_AGGRESSIVE);
   assert(config_field_set_value(&c, econ, "off") == 0 && c.economizer_tier == ECON_TIER_OFF);
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

   /* --- defaults: economizer tier SAFE, Coordinate Closet on --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg); /* missing file -> defaults */
      assert(cfg.economizer_tier == ECON_TIER_SAFE);
      assert(cfg.coord_closet_enabled == 1);
   }

   /* --- economizer tier: save/load round-trip (string form) --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      assert(cfg.economizer_tier == ECON_TIER_SAFE); /* default is safe */
      cfg.economizer_tier = ECON_TIER_AGGRESSIVE;
      assert(config_save(&cfg) == 0);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(cfg2.economizer_tier ==
             ECON_TIER_AGGRESSIVE); /* persisted as economizer: aggressive */

      /* off also round-trips (non-default); safe is the default and is not persisted. */
      cfg.economizer_tier = ECON_TIER_OFF;
      assert(config_save(&cfg) == 0);
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(cfg2.economizer_tier == ECON_TIER_OFF);
   }

   /* --- deprecated `economizer: {enabled,aggressive}` object form still maps to a tier --- */
   {
      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      cfg.economizer_tier = ECON_TIER_SAFE;
      cJSON *root = cJSON_Parse("{\"economizer\":{\"enabled\":false}}");
      assert(root);
      config_parse_economizer_section(&cfg, root);
      assert(cfg.economizer_tier == ECON_TIER_OFF); /* enabled:false -> off */
      cJSON_Delete(root);

      root = cJSON_Parse("{\"economizer\":{\"enabled\":true,\"aggressive\":true}}");
      assert(root);
      config_parse_economizer_section(&cfg, root);
      assert(cfg.economizer_tier == ECON_TIER_AGGRESSIVE);
      cJSON_Delete(root);
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

      /* modules.economizer:false is an authoritative hard-kill over the tier; otherwise the
       * tier decides. */
      config_t ec;
      memset(&ec, 0, sizeof(ec));
      ec.economizer_tier = ECON_TIER_SAFE;
      ec.module_economizer = 0; /* hard kill regardless of tier */
      assert(econ_reduction_master_on(&ec) == 0);
      ec.module_economizer = 1; /* not killed -> tier decides */
      assert(econ_reduction_master_on(&ec) == 1);
      ec.module_economizer = -1; /* unspecified -> tier decides */
      assert(econ_reduction_master_on(&ec) == 1);
      ec.economizer_tier = ECON_TIER_OFF;
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
