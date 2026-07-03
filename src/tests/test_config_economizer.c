/* test_config_economizer.c: defaults + save/load round-trip for the context
 * economizer (reduce.*) and Coordinate Closet config. Kept separate from
 * test_config.c, which is at the build-integrity line-count limit. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aimee.h"
#include "config_sections.h"
#include "aimee_home.h"
#include "platform_path.h"
#include "platform_test_util.h"

int main(void)
{
   printf("config_economizer: ");

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
