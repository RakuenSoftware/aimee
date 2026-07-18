/* test_config_economizer.c: defaults + save/load round-trip for the context
 * economizer (reduce.*) and Coordinate Closet config. Kept separate from
 * test_config.c, which is at the build-integrity line-count limit. */
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

/* Effective safe-tier lever (command_filter): master && individual. Mirrors
 * tool_condense_enabled without pulling the CORE object into this test's link set. */
static int cf_effective(const config_t *c)
{
   return econ_reduction_master_on(c) && c->reduce_command_filter;
}

/* P3 two-tier resolution: master-kill, aggressive ceiling, and BACK-COMPAT (default
 * switches must reproduce pre-P3 effective values). Pure — no file I/O. */
static void test_two_tier_resolution(void)
{
   config_t c;
   memset(&c, 0, sizeof c);
   c.module_economizer = -1; /* unspecified -> tier is authoritative */
   c.reduce_command_filter = 1;

   /* OFF tier: no economization at all. */
   c.economizer_tier = ECON_TIER_OFF;
   assert(econ_tier(&c) == ECON_TIER_OFF);
   assert(econ_reduction_master_on(&c) == 0);
   assert(cf_effective(&c) == 0); /* command_filter suppressed with reduction off */
   assert(econ_gateway_mutate_on(&c) == 0);

   /* SAFE tier: reduction on, but no live gateway mutation. */
   c.economizer_tier = ECON_TIER_SAFE;
   assert(econ_reduction_master_on(&c) == 1);
   assert(cf_effective(&c) == 1);
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
   const config_field_t *hot = config_field_lookup("economizer.aggressive");
   const config_field_t *rst = config_field_lookup("db2_url"); /* startup: pg pool */
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
   test_two_tier_resolution();
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

   /* --- defaults: economizer DEFAULT-ON on the delegate seam, gateway OFF --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg); /* missing file -> defaults */
      assert(cfg.reduce_measure_enabled == 1);
      assert(cfg.reduce_delegate_seam == 1);
      assert(cfg.reduce_history_fold == 1);
      assert(cfg.reduce_compress == 1);
      assert(cfg.coord_closet_enabled == 1);
      assert(cfg.reduce_gateway_seam == 0); /* primary path off pending mutation build */
      assert(cfg.reduce_freeze_guard_enabled == 1);
      assert(cfg.reduce_freeze_guard_horizon == 1);
   }

   /* --- bulk opt-out: every default-ON lever + closet flipped OFF must round-trip
    * (config_save persists only the non-default OFF state) --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      cfg.reduce_measure_enabled = 0;
      cfg.reduce_delegate_seam = 0;
      cfg.reduce_history_fold = 0;
      cfg.reduce_compress = 0;
      cfg.coord_closet_enabled = 0;
      cfg.reduce_freeze_guard_enabled = 0;
      cfg.reduce_freeze_guard_horizon = 3;
      assert(config_save(&cfg) == 0);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(cfg2.reduce_measure_enabled == 0);
      assert(cfg2.reduce_delegate_seam == 0);
      assert(cfg2.reduce_history_fold == 0);
      assert(cfg2.reduce_compress == 0);
      assert(cfg2.coord_closet_enabled == 0);
      assert(cfg2.reduce_freeze_guard_enabled == 0);
      assert(cfg2.reduce_freeze_guard_horizon == 3);
   }

   /* --- partial opt-out + closet tuning: one lever OFF with siblings at default ON,
    * closet ON but tuned (budget/ratio/denylist) must round-trip byte-equal --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      /* pin siblings ON deterministically (config_load reads the prior block's file) */
      cfg.reduce_measure_enabled = 1;
      cfg.reduce_delegate_seam = 1;
      cfg.reduce_history_fold = 0; /* the single opt-out */
      cfg.reduce_compress = 1;
      cfg.reduce_freeze_guard_enabled = 1;
      cfg.reduce_freeze_guard_horizon = 1;
      cfg.coord_closet_enabled = 1;
      cfg.coord_closet_budget_bytes = 4096;
      cfg.coord_closet_max_ratio_pct = 15;
      snprintf(cfg.coord_closet_denylist, sizeof(cfg.coord_closet_denylist), "secretword");
      assert(config_save(&cfg) == 0);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(cfg2.reduce_history_fold == 0);    /* the single opt-out survived */
      assert(cfg2.reduce_measure_enabled == 1); /* siblings stayed on */
      assert(cfg2.reduce_delegate_seam == 1);
      assert(cfg2.reduce_compress == 1);
      assert(cfg2.coord_closet_enabled == 1); /* closet on while tuned */
      assert(cfg2.coord_closet_budget_bytes == 4096);
      assert(cfg2.coord_closet_max_ratio_pct == 15);
      assert(strcmp(cfg2.coord_closet_denylist, "secretword") == 0);
   }

   /* --- gateway mutation: defaults --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      assert(cfg.reduce_gateway_mutate == 0);
      assert(cfg.reduce_gateway_session_disable_ttl_ms == 3600000);
      assert(cfg.reduce_gateway_seam_explicit == 0);
   }

   /* --- mutate=1 auto-enables the shadow seam IN MEMORY + does NOT persist the
    * synthesized gateway_seam: save {mutate:1} (no seam), reload, and confirm seam
    * was re-synthesized (==1) while seam_explicit stayed 0 (proving the key was not
    * written to disk — a persisted key would set explicit on parse). --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      cfg.reduce_gateway_mutate = 1;
      cfg.reduce_gateway_seam = 0;
      cfg.reduce_gateway_seam_explicit = 0;
      assert(config_save(&cfg) == 0);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(cfg2.reduce_gateway_mutate == 1);        /* mutate persisted */
      assert(cfg2.reduce_gateway_seam == 1);          /* auto-enabled in memory */
      assert(cfg2.reduce_gateway_seam_explicit == 0); /* synthesized, NOT persisted */
   }

   /* --- an explicitly-set gateway_seam DOES persist (and carries explicit=1) --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      cfg.reduce_gateway_mutate = 0;
      cfg.reduce_gateway_seam = 1;
      cfg.reduce_gateway_seam_explicit = 1;
      assert(config_save(&cfg) == 0);

      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(cfg2.reduce_gateway_seam == 1);
      assert(cfg2.reduce_gateway_seam_explicit == 1);
   }

   /* --- non-default disable TTL round-trips; default (1h) is not persisted --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      cfg.reduce_gateway_session_disable_ttl_ms = 7200000; /* 2h override */
      assert(config_save(&cfg) == 0);
      static config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(cfg2.reduce_gateway_session_disable_ttl_ms == 7200000);
   }

   /* --- consistency hook is a pure in-memory normalization --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      cfg.reduce_gateway_mutate = 1;
      cfg.reduce_gateway_seam = 0;
      cfg.reduce_gateway_seam_explicit = 0;
      config_apply_reduce_consistency(&cfg);
      assert(cfg.reduce_gateway_seam == 1);
      assert(cfg.reduce_gateway_seam_explicit == 0);
      /* idempotent + no downgrade when mutate is off */
      memset(&cfg, 0, sizeof(cfg));
      cfg.reduce_gateway_mutate = 0;
      cfg.reduce_gateway_seam = 0;
      config_apply_reduce_consistency(&cfg);
      assert(cfg.reduce_gateway_seam == 0);
      /* seam:false + mutate:true — synthesis must CLEAR explicit so config_save
       * never rewrites the operator's on-disk false to true. */
      memset(&cfg, 0, sizeof(cfg));
      cfg.reduce_gateway_mutate = 1;
      cfg.reduce_gateway_seam = 0;
      cfg.reduce_gateway_seam_explicit = 1; /* operator wrote gateway_seam: false */
      config_apply_reduce_consistency(&cfg);
      assert(cfg.reduce_gateway_seam == 1);          /* mutate wins in memory */
      assert(cfg.reduce_gateway_seam_explicit == 0); /* but not persistable */
   }

   /* --- startup-fatal TTL validation: <=0 rejected, >0 accepted --- */
   {
      static config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      char err[256];
      cfg.reduce_gateway_session_disable_ttl_ms = 3600000;
      assert(config_reduce_validate(&cfg, err, sizeof(err)) == 0);
      cfg.reduce_gateway_session_disable_ttl_ms = 0;
      assert(config_reduce_validate(&cfg, err, sizeof(err)) != 0);
      cfg.reduce_gateway_session_disable_ttl_ms = -5;
      assert(config_reduce_validate(&cfg, err, sizeof(err)) != 0);
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
