/* curator_profile.h: install-today hardware-aware curator profile picker.
 *
 * Selects the optimal [curator.local] backend and model configuration based
 * on detected hardware. The four profiles match the proposal spec:
 *   cpu      — default; Gemma 4 E4B via scripts/curator-extract.py
 *   gpu      — ≥24 GB discrete VRAM; Qwen3.6-35B-A3B via llama.cpp
 *   endpoint — caller-supplied OpenAI-compatible URL; no local inference
 *   disabled — no local inference; kb_curator_extract_* disabled
 *
 * See docs/proposals/accepted/aimee-kb-service-and-public-api.md
 * (Phase 2: Install-today profile picker). */
#ifndef DEC_CURATOR_PROFILE_H
#define DEC_CURATOR_PROFILE_H 1

#include <stddef.h>
#include "config.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* Curator backend kinds. */
   typedef enum
   {
      CURATOR_BACKEND_CPU = 0,      /* default: Gemma 4 E4B, scripts/curator-extract.py */
      CURATOR_BACKEND_GPU = 1,      /* ≥24 GB VRAM: Qwen3.6-35B-A3B via llama.cpp */
      CURATOR_BACKEND_ENDPOINT = 2, /* OpenAI-compatible endpoint (kb_url / endpoint field) */
      CURATOR_BACKEND_DISABLED = 3  /* no local inference */
   } curator_backend_t;

   /* VRAM threshold above which the GPU backend is selected. */
#define CURATOR_GPU_VRAM_THRESHOLD_MB 24000

   /* Resolved profile returned by curator_profile_select. */
   typedef struct
   {
      curator_backend_t backend;
      char model[256];           /* model identifier or path */
      char extract_command[512]; /* kb_curator_extract_command value */
      char endpoint_url[512];    /* non-empty for CURATOR_BACKEND_ENDPOINT */
      int docs_enabled;          /* 0 = disabled */
      int code_enabled;          /* 0 = disabled */
   } curator_profile_t;

   /* String ↔ enum helpers. Return a static string; never NULL. */
   const char *curator_backend_name(curator_backend_t b);
   curator_backend_t curator_backend_parse(const char *s);

   /* Select the best profile given detected hardware.
    * vram_mb: detected discrete VRAM in MiB (0 if no GPU detected).
    * endpoint_url: non-NULL and non-empty → forces CURATOR_BACKEND_ENDPOINT.
    * Returns the selected profile. Output-shape is consistent across all
    * backends (extract_command always points at the same sidecar script;
    * model varies).  */
   curator_profile_t curator_profile_select(int vram_mb, const char *endpoint_url);

   /* Apply a curator_profile_t to config fields (does not write the INI
    * file — caller must call config_save or equivalent).
    * Returns 0 on success, -1 if cfg is NULL. */
   int curator_profile_apply(curator_profile_t *profile);

   /* Detect hardware (calls hardware_probe_cached_or_detect) and select
    * the appropriate profile. Convenience wrapper over the two functions
    * above. endpoint_url may be NULL or empty to allow auto-detection.
    * Returns the selected profile. */
   curator_profile_t curator_profile_detect_and_select(const char *endpoint_url);

   /* Write a human-readable summary of the profile to buf (len bytes).
    * Returns buf. */
   char *curator_profile_describe(const curator_profile_t *p, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* DEC_CURATOR_PROFILE_H */
