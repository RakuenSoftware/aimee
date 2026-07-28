/* test_curator_profile.c — unit tests for the curator profile picker.
 *
 * Tests:
 *   1. CPU backend selected by default (0 VRAM, no endpoint).
 *   2. GPU backend selected when VRAM >= 24 GB.
 *   3. GPU NOT selected below the 24 GB threshold.
 *   4. Endpoint backend selected when endpoint_url is provided.
 *   5. Endpoint takes priority over GPU.
 *   6. curator_backend_name / curator_backend_parse round-trip.
 *   7. curator_profile_describe produces a non-empty string.
 *   8. curator_profile_apply updates config fields.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "config.h"
#include "headers/curator_profile.h"

/* ---- 1. CPU default (no GPU, no endpoint) ---- */
static void test_cpu_default(void)
{
   curator_profile_t p = curator_profile_select(0, NULL);
   assert(p.backend == CURATOR_BACKEND_CPU);
   assert(p.docs_enabled == 1);
   assert(p.code_enabled == 1);
   assert(strstr(p.extract_command, "curator-extract") != NULL);
   assert(strstr(p.model, "gemma") != NULL);
   printf("  cpu_default: ok\n");
}

/* ---- 2. GPU selected at or above threshold ---- */
static void test_gpu_at_threshold(void)
{
   curator_profile_t p = curator_profile_select(CURATOR_GPU_VRAM_THRESHOLD_MB, NULL);
   assert(p.backend == CURATOR_BACKEND_GPU);
   assert(strstr(p.model, "qwen") != NULL || strstr(p.model, "3.6") != NULL);
   assert(p.docs_enabled == 1);
   printf("  gpu_at_threshold: ok\n");
}

static void test_gpu_above_threshold(void)
{
   curator_profile_t p = curator_profile_select(48000, NULL);
   assert(p.backend == CURATOR_BACKEND_GPU);
   printf("  gpu_above_threshold: ok\n");
}

/* ---- 3. GPU not selected below threshold ---- */
static void test_gpu_below_threshold(void)
{
   curator_profile_t p = curator_profile_select(CURATOR_GPU_VRAM_THRESHOLD_MB - 1, NULL);
   assert(p.backend == CURATOR_BACKEND_CPU);
   printf("  gpu_below_threshold: ok\n");
}

static void test_gpu_small_vram(void)
{
   curator_profile_t p = curator_profile_select(8000, NULL);
   assert(p.backend == CURATOR_BACKEND_CPU);
   printf("  gpu_small_vram: ok\n");
}

/* ---- 4. Endpoint when URL provided ---- */
static void test_endpoint_url(void)
{
   curator_profile_t p = curator_profile_select(0, "https://api.openai.com/v1");
   assert(p.backend == CURATOR_BACKEND_ENDPOINT);
   assert(strcmp(p.endpoint_url, "https://api.openai.com/v1") == 0);
   assert(p.docs_enabled == 1);
   printf("  endpoint_url: ok\n");
}

/* ---- 5. Endpoint takes priority over GPU ---- */
static void test_endpoint_beats_gpu(void)
{
   curator_profile_t p = curator_profile_select(48000, "http://localhost:11434/v1");
   assert(p.backend == CURATOR_BACKEND_ENDPOINT);
   printf("  endpoint_beats_gpu: ok\n");
}

/* Empty endpoint string → falls through to hw detection. */
static void test_empty_endpoint_ignored(void)
{
   curator_profile_t p = curator_profile_select(0, "");
   assert(p.backend == CURATOR_BACKEND_CPU);
   printf("  empty_endpoint_ignored: ok\n");
}

/* ---- 6. backend_name / parse round-trip ---- */
static void test_backend_round_trip(void)
{
   assert(curator_backend_parse(curator_backend_name(CURATOR_BACKEND_CPU)) == CURATOR_BACKEND_CPU);
   assert(curator_backend_parse(curator_backend_name(CURATOR_BACKEND_GPU)) == CURATOR_BACKEND_GPU);
   assert(curator_backend_parse(curator_backend_name(CURATOR_BACKEND_ENDPOINT)) ==
          CURATOR_BACKEND_ENDPOINT);
   assert(curator_backend_parse(curator_backend_name(CURATOR_BACKEND_DISABLED)) ==
          CURATOR_BACKEND_DISABLED);
   /* Unknown string → cpu default. */
   assert(curator_backend_parse("unknown") == CURATOR_BACKEND_CPU);
   assert(curator_backend_parse(NULL) == CURATOR_BACKEND_CPU);
   printf("  backend_round_trip: ok\n");
}

/* ---- 7. describe produces non-empty string ---- */
static void test_describe_nonempty(void)
{
   curator_profile_t p = curator_profile_select(0, NULL);
   char buf[256] = "";
   char *ret = curator_profile_describe(&p, buf, sizeof(buf));
   assert(ret == buf);
   assert(buf[0] != '\0');
   assert(strstr(buf, "backend=cpu") != NULL);
   printf("  describe_nonempty: ok\n");
}

/* ---- 8. apply updates config fields ---- */
static void test_apply_config(void)
{
   /* apply now PERSISTS through the config module rather than editing a caller's
    * struct, so the assertions read back through the accessors. The suite runs
    * with HOME/TMPDIR pointed at a throwaway dir, so this writes to a scratch
    * config, not the developer's. */
   curator_profile_t p = curator_profile_select(0, NULL);
   assert(curator_profile_apply(&p) == 0);
   assert(config_kb_curator_extract_docs_enabled() == 1);
   assert(config_kb_curator_extract_code_enabled() == 1);
   assert(strstr(config_kb_curator_extract_command(), "curator-extract") != NULL);

   /* Disabled profile turns off extraction. */
   curator_profile_t dis;
   memset(&dis, 0, sizeof(dis));
   dis.backend = CURATOR_BACKEND_DISABLED;
   assert(curator_profile_apply(&dis) == 0);
   assert(config_kb_curator_extract_docs_enabled() == 0);
   assert(config_kb_curator_extract_code_enabled() == 0);

   printf("  apply_config: ok\n");
}

int main(void)
{
   printf("curator_profile:\n");
   test_cpu_default();
   test_gpu_at_threshold();
   test_gpu_above_threshold();
   test_gpu_below_threshold();
   test_gpu_small_vram();
   test_endpoint_url();
   test_endpoint_beats_gpu();
   test_empty_endpoint_ignored();
   test_backend_round_trip();
   test_describe_nonempty();
   test_apply_config();
   printf("All curator_profile tests passed.\n");
   return 0;
}
