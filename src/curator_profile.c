/* curator_profile.c: install-today hardware-aware curator profile picker.
 *
 * See src/headers/curator_profile.h. */

#include "aimee.h"
#include "headers/curator_profile.h"
#include "hardware_probe.h"
#include "config.h"

#include <stdio.h>
#include <string.h>

/* Default sidecar script used across all local-inference backends. */
#define CURATOR_EXTRACT_SCRIPT "scripts/curator-extract.py"

/* ── enum helpers ─────────────────────────────────────────────────────────── */

const char *curator_backend_name(curator_backend_t b)
{
   switch (b)
   {
   case CURATOR_BACKEND_CPU:
      return "cpu";
   case CURATOR_BACKEND_GPU:
      return "gpu";
   case CURATOR_BACKEND_ENDPOINT:
      return "endpoint";
   case CURATOR_BACKEND_DISABLED:
      return "disabled";
   }
   return "cpu";
}

curator_backend_t curator_backend_parse(const char *s)
{
   if (!s)
      return CURATOR_BACKEND_CPU;
   if (strcmp(s, "gpu") == 0)
      return CURATOR_BACKEND_GPU;
   if (strcmp(s, "endpoint") == 0)
      return CURATOR_BACKEND_ENDPOINT;
   if (strcmp(s, "disabled") == 0)
      return CURATOR_BACKEND_DISABLED;
   return CURATOR_BACKEND_CPU;
}

/* ── profile selection ────────────────────────────────────────────────────── */

curator_profile_t curator_profile_select(int vram_mb, const char *endpoint_url)
{
   curator_profile_t p;
   memset(&p, 0, sizeof(p));
   snprintf(p.extract_command, sizeof(p.extract_command), "%s", CURATOR_EXTRACT_SCRIPT);

   /* Endpoint takes priority: caller explicitly configured an external service. */
   if (endpoint_url && endpoint_url[0])
   {
      p.backend = CURATOR_BACKEND_ENDPOINT;
      snprintf(p.endpoint_url, sizeof(p.endpoint_url), "%s", endpoint_url);
      snprintf(p.model, sizeof(p.model), "gpt-4o-mini"); /* default; caller may override */
      p.docs_enabled = 1;
      p.code_enabled = 1;
      return p;
   }

   /* GPU path: discrete VRAM ≥ CURATOR_GPU_VRAM_THRESHOLD_MB. */
   if (vram_mb >= CURATOR_GPU_VRAM_THRESHOLD_MB)
   {
      p.backend = CURATOR_BACKEND_GPU;
      snprintf(p.model, sizeof(p.model), "qwen3.6-35b-a3b");
      p.docs_enabled = 1;
      p.code_enabled = 1;
      return p;
   }

   /* CPU path: default, always works. */
   p.backend = CURATOR_BACKEND_CPU;
   snprintf(p.model, sizeof(p.model), "gemma-4-e4b");
   p.docs_enabled = 1;
   p.code_enabled = 1;
   return p;
}

/* ── config application ───────────────────────────────────────────────────── */

/* Persist a profile's curator settings.
 *
 * This took a legacy_config_record * and mutated it in place, leaving the caller to own the
 * load and the save. Writing through the config module's setters means no
 * caller needs the struct — and the settings are actually persisted rather than
 * left in a caller's copy that may never be saved. */
int curator_profile_apply(curator_profile_t *profile)
{
   if (!profile)
      return -1;

   int docs = profile->docs_enabled;
   int code = profile->code_enabled;
   /* Disabled turns off both, whatever the profile's individual flags say. */
   if (profile->backend == CURATOR_BACKEND_DISABLED)
      docs = code = 0;

   if (config_set_kb_curator_extract_docs_enabled(docs) != 0)
      return -1;
   if (config_set_kb_curator_extract_code_enabled(code) != 0)
      return -1;
   if (profile->extract_command[0] &&
       config_set_kb_curator_extract_command(profile->extract_command) != 0)
      return -1;
   return 0;
}

/* ── detect-and-select convenience ───────────────────────────────────────── */

curator_profile_t curator_profile_detect_and_select(const char *endpoint_url)
{
   hardware_probe_result_t hw;
   hardware_probe_result_init(&hw);
   (void)hardware_probe_cached_or_detect(&hw);
   int vram_mb = (hw.detected) ? hw.vram_mb : 0;
   return curator_profile_select(vram_mb, endpoint_url);
}

/* ── describe ─────────────────────────────────────────────────────────────── */

char *curator_profile_describe(const curator_profile_t *p, char *buf, size_t len)
{
   if (!p || !buf || len == 0)
      return buf;
   if (p->backend == CURATOR_BACKEND_ENDPOINT)
      snprintf(buf, len, "backend=%s model=%s endpoint=%s docs=%d code=%d",
               curator_backend_name(p->backend), p->model, p->endpoint_url, p->docs_enabled,
               p->code_enabled);
   else
      snprintf(buf, len, "backend=%s model=%s extract_command=%s docs=%d code=%d",
               curator_backend_name(p->backend), p->model, p->extract_command, p->docs_enabled,
               p->code_enabled);
   return buf;
}
