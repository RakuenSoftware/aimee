/* config_econ.c -- the economizer's view of config.
 *
 * Split out of config.c, which crossed the 2500-line hard limit. These belong
 * together and belong away from the loader: they are policy over already-parsed
 * fields (which mode a preset implies, which knobs a mode turns on), not part of
 * reading, validating, or publishing the config.
 *
 * Both forms are kept deliberately. econ_mode(cfg)/econ_preset(cfg, ...) are for
 * the config module's own callers, which already hold the struct; the _current
 * forms read the live config so nobody outside has to.
 */
#include <stdlib.h>
#include <string.h>

#include "config.h"

int econ_mode(const config_t *cfg)
{
   if (!cfg)
      return ECON_MODE_OFF;
   /* modules.economizer:false is an authoritative hard kill. */
   if (cfg->module_economizer == 0)
      return ECON_MODE_OFF;
   return cfg->economizer_mode;
}

const char *econ_mode_name(int mode)
{
   switch (mode)
   {
   case ECON_MODE_OFF:
      return "off";
   case ECON_MODE_AGGRESSIVE:
      return "aggressive";
   case ECON_MODE_SAFE:
   default:
      return "safe";
   }
}

int econ_mode_parse(const char *s)
{
   if (s && strcasecmp(s, "off") == 0)
      return ECON_MODE_OFF;
   if (s && strcasecmp(s, "safe") == 0)
      return ECON_MODE_SAFE;
   if (s && strcasecmp(s, "aggressive") == 0)
      return ECON_MODE_AGGRESSIVE;
   return -1;
}

const char *guardrails_semantic_mode_name(int mode)
{
   switch (mode)
   {
   case GSEM_MODE_DRY_RUN:
      return "dry_run";
   case GSEM_MODE_ADVISORY:
      return "advisory";
   case GSEM_MODE_ENFORCE:
      return "enforce";
   case GSEM_MODE_OFF:
   default:
      return "off";
   }
}

int guardrails_semantic_mode_parse(const char *s)
{
   if (!s)
      return GSEM_MODE_OFF;
   if (strcasecmp(s, "dry_run") == 0 || strcasecmp(s, "dryrun") == 0)
      return GSEM_MODE_DRY_RUN;
   if (strcasecmp(s, "advisory") == 0)
      return GSEM_MODE_ADVISORY;
   if (strcasecmp(s, "enforce") == 0)
      return GSEM_MODE_ENFORCE;
   return GSEM_MODE_OFF; /* "off"/"0"/"false"/unknown -> fail-safe off */
}

int econ_reduction_master_on(const config_t *cfg)
{
   return econ_mode(cfg) != ECON_MODE_OFF;
}

/* Live-config forms, for callers outside this module. Same policy as the
 * config_t forms above, read through accessors so a caller never materialises
 * the struct to ask a two-field question. The config_t forms stay: they are
 * inside the config module, and econ_mode is a PURE function of two fields --
 * test_config_economizer exercises the whole resolution table with no I/O,
 * which is worth keeping. Mirrors config_embedder_command / _current. */
int econ_mode_current(void)
{
   if (config_module_economizer() == 0)
      return ECON_MODE_OFF; /* modules.economizer:false is a hard kill */
   return config_economizer_mode();
}

int econ_gateway_mutate_on_current(void)
{
   return econ_mode_current() == ECON_MODE_AGGRESSIVE;
}

void econ_preset_current(econ_preset_t *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof *out);
   int mode = econ_mode_current();
   if (mode == ECON_MODE_OFF)
      return;
   out->json_compact = 1;
   if (mode == ECON_MODE_AGGRESSIVE)
   {
      out->history_fold = 1;
      out->compress = 1;
      out->command_filter = 1;
      out->freeze_guard_horizon = 1;
      out->gateway_seam = 1;
      out->gateway_session_disable_ttl_ms = 3600000;
   }
}

int config_module_enabled(int config_tristate, int env_default)
{
   /* (Future tier 1: admin/governance FORCE would short-circuit here.) Tier 2: an explicit
    * user config tristate (0/1) is canonical. Tier 3: -1 (unspecified) falls back to the
    * deprecated env default. See config.h for the full precedence contract. */
   if (config_tristate == 0 || config_tristate == 1)
      return config_tristate;
   return env_default ? 1 : 0;
}

int econ_gateway_mutate_on(const config_t *cfg)
{
   return econ_mode(cfg) == ECON_MODE_AGGRESSIVE;
}

void econ_preset(const config_t *cfg, econ_preset_t *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof *out);
   int mode = econ_mode(cfg);
   if (mode == ECON_MODE_OFF)
      return;
   out->json_compact = 1;
   if (mode == ECON_MODE_AGGRESSIVE)
   {
      out->history_fold = 1;
      out->compress = 1;
      out->command_filter = 1;
      out->freeze_guard_horizon = 1;
      out->gateway_seam = 1;
      out->gateway_session_disable_ttl_ms = 3600000;
   }
}
