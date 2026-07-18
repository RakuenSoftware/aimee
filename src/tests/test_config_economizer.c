/* test_config_economizer.c: defaults + save/load round-trip for the context
 * economizer (the `economizer` tier control) and the Coordinate Closet config.
 * The old per-lever reduce.* surface was removed — the tier drives every reducer.
 * Kept separate from test_config.c, which is at the build-integrity line limit. */
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

/* Tier resolution: OFF/SAFE/AGGRESSIVE map to the econ_* accessors, modules.economizer:false
 * is an authoritative hard-kill, and the string parser/name round-trips. Pure — no file I/O. */
static void test_tier_resolution(void)
{
   config_t c;
   memset(&c, 0, sizeof c);
   c.module_economizer = -1; /* unspecified -> tier is authoritative */

   /* OFF tier: no economization at all. */
   c.economizer_tier = ECON_TIER_OFF;
   assert(econ_tier(&c) == ECON_TIER_OFF);
   assert(econ_reduction_master_on(&c) == 0);
   assert(econ_gateway_mutate_on(&c) == 0);

   /* SAFE tier: reduction on, but no live gateway mutation. */
   c.economizer_tier = ECON_TIER_SAFE;
   assert(econ_reduction_master_on(&c) == 1);
   assert(econ_gateway_mutate_on(&c) == 0); /* live mutation is aggressive-only */

   /* AGGRESSIVE tier: reduction on + live mutation (OpenAI-only enforced at the call site). */
   c.economizer_tier = ECON_TIER_AGGRESSIVE;
   assert(econ_reduction_master_on(&c) == 1);
   assert(econ_gateway_mutate_on(&c) == 1);

   /* modules.economizer:false is an authoritative hard-kill over any tier. */
   c.module_economizer = 0;
   assert(econ_tier(&c) == ECON_TIER_OFF);
   assert(econ_reduction_master_on(&c) == 0);
   assert(econ_gateway_mutate_on(&c) == 0);

   /* the tier parser + names round-trip. */
   assert(econ_tier_parse("off") == ECON_TIER_OFF);
   assert(econ_tier_parse("safe") == ECON_TIER_SAFE);
   assert(econ_tier_parse("aggressive") == ECON_TIER_AGGRESSIVE);
   assert(econ_tier_parse("bogus") == ECON_TIER_SAFE); /* unknown -> safe */
   assert(strcmp(econ_tier_name(ECON_TIER_AGGRESSIVE), "aggressive") == 0);

   /* NULL-safe */
   assert(econ_reduction_master_on(NULL) == 0);
   assert(econ_gateway_mutate_on(NULL) == 0);
}

/* live-config-reload P2: reload-class classification + verdict. */
static void test_reload_class(void)
{
   const config_field_t *hot = config_field_lookup("autonomous"); /* live via snapshot: HOT */
   const config_field_t *rst = config_field_lookup("db2_url");    /* startup: pg pool */
   const config_field_t *aut =
       config_field_lookup("autonomy.skeptics"); /* live via snapshot: HOT */
   assert(hot && hot->reload_class == RELOAD_HOT);
   assert(rst && rst->reload_class == RELOAD_RESTART);
   assert(aut && aut->reload_class == RELOAD_HOT);
   assert(strstr(config_field_reload_verdict(rst), "restart"));
   assert(strcmp(config_field_reload_verdict(hot), "applied live") == 0);
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

   /* --- defaults: SAFE tier + Coordinate Closet on --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg); /* missing file -> defaults */
      assert(cfg.economizer_tier == ECON_TIER_SAFE);
      assert(cfg.coord_closet_enabled == 1);
      assert(cfg.module_economizer == -1); /* unspecified -> tier authoritative */
   }

   /* --- Coordinate Closet round-trip: off-state + tuned (budget/ratio/denylist) must
    * persist byte-equal. The closet is a separate config family from the economizer tier;
    * it survives the reduce.* removal untouched. --- */
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

   /* --- economizer tier: default + save/load round-trip (string form) --- */
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

   /* --- deprecated object form {enabled,aggressive} maps onto the tier (back-compat).
    * Asserted directly on the pure parser — config_save always writes the canonical string
    * form, so this is the only path that still exercises the object mapping. --- */
   {
      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      cfg.economizer_tier = ECON_TIER_SAFE;
      cJSON *root = cJSON_CreateObject();
      cJSON *e = cJSON_AddObjectToObject(root, "economizer");
      cJSON_AddBoolToObject(e, "enabled", 1);
      cJSON_AddBoolToObject(e, "aggressive", 1);
      config_parse_reduce_section(&cfg, root);
      assert(cfg.economizer_tier == ECON_TIER_AGGRESSIVE); /* enabled+aggressive -> AGGRESSIVE */
      cJSON_Delete(root);

      memset(&cfg, 0, sizeof(cfg));
      root = cJSON_CreateObject();
      e = cJSON_AddObjectToObject(root, "economizer");
      cJSON_AddBoolToObject(e, "enabled", 0);
      cJSON_AddBoolToObject(e, "aggressive", 1);
      config_parse_reduce_section(&cfg, root);
      assert(cfg.economizer_tier == ECON_TIER_OFF); /* enabled:false -> OFF regardless */
      cJSON_Delete(root);
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
