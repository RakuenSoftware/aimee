#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aimee.h"
#include "model_registry.h"
#include "models_dev.h"

static void test_alias_resolve(void)
{
   model_info_t info;

   /* Anthropic aliases */
   assert(model_alias_resolve("opus", &info) == 1);
   assert(strcmp(info.provider, "anthropic") == 0);
   assert(strstr(info.model_id, "opus") != NULL);

   assert(model_alias_resolve("sonnet", &info) == 1);
   assert(strcmp(info.provider, "anthropic") == 0);
   assert(strstr(info.model_id, "sonnet") != NULL);

   assert(model_alias_resolve("haiku", &info) == 1);
   assert(strcmp(info.provider, "anthropic") == 0);

   /* Case-insensitive */
   assert(model_alias_resolve("OPUS", &info) == 1);
   assert(strcmp(info.provider, "anthropic") == 0);

   assert(model_alias_resolve("Sonnet", &info) == 1);
   assert(strcmp(info.provider, "anthropic") == 0);

   /* OpenAI aliases */
   assert(model_alias_resolve("gpt4o", &info) == 1);
   assert(strcmp(info.provider, "openai") == 0);
   assert(strcmp(info.model_id, "gpt-4o") == 0);

   assert(model_alias_resolve("gpt4", &info) == 1);
   assert(strcmp(info.provider, "openai") == 0);

   /* Gemini aliases */
   assert(model_alias_resolve("gemini", &info) == 1);
   assert(strcmp(info.provider, "gemini") == 0);

   assert(model_alias_resolve("gemini-pro", &info) == 1);
   assert(strcmp(info.provider, "gemini") == 0);

   /* Mistral aliases */
   assert(model_alias_resolve("codestral", &info) == 1);
   assert(strcmp(info.provider, "mistral") == 0);
   assert(strcmp(info.model_id, "codestral-latest") == 0);

   assert(model_alias_resolve("mistral-large", &info) == 1);
   assert(strcmp(info.provider, "mistral") == 0);
   assert(strcmp(info.model_id, "mistral-large-latest") == 0);

   assert(model_alias_resolve("mistral-small", &info) == 1);
   assert(strcmp(info.provider, "mistral") == 0);
   assert(strcmp(info.model_id, "mistral-small-latest") == 0);

   /* Unknown alias returns 0 */
   assert(model_alias_resolve("nonexistent-model-xyz", &info) == 0);
   assert(model_alias_resolve("", &info) == 0);
   assert(model_alias_resolve(NULL, &info) == 0);
}

static void test_provider_detect(void)
{
   /* Anthropic models */
   assert(strcmp(model_detect_provider("claude-opus-4-6"), "anthropic") == 0);
   assert(strcmp(model_detect_provider("claude-sonnet-4-6"), "anthropic") == 0);
   assert(strcmp(model_detect_provider("claude-3-5-haiku"), "anthropic") == 0);

   /* OpenAI models */
   assert(strcmp(model_detect_provider("gpt-4o"), "openai") == 0);
   assert(strcmp(model_detect_provider("gpt-4-turbo"), "openai") == 0);
   assert(strcmp(model_detect_provider("gpt-3.5-turbo"), "openai") == 0);
   assert(strcmp(model_detect_provider("o1"), "openai") == 0);

   /* Gemini models */
   assert(strcmp(model_detect_provider("gemini-1.5-pro"), "gemini") == 0);
   assert(strcmp(model_detect_provider("gemini-2.5-pro"), "gemini") == 0);

   /* Mistral models */
   assert(strcmp(model_detect_provider("mistral-large-latest"), "mistral") == 0);
   assert(strcmp(model_detect_provider("codestral-latest"), "mistral") == 0);
   assert(strcmp(model_detect_provider("MiniMax-M2.7"), "minimax") == 0);

   /* Unknown: fallback to openai-compatible */
   assert(strcmp(model_detect_provider("unknown-compatible-model"), "openai") == 0);

   /* NULL input */
   assert(model_detect_provider(NULL) == NULL);
   assert(model_detect_provider("") == NULL);
}

static void test_context_window(void)
{
   /* Anthropic: 200k */
   assert(model_context_window("claude-opus-4-6") == 200000);
   assert(model_context_window("claude-sonnet-4-6") == 200000);
   assert(model_context_window("claude-haiku-4-5-20251001") == 200000);
   assert(model_context_window("claude-3-5-sonnet-20241022") == 200000);

   /* Claude 2: 100k */
   assert(model_context_window("claude-2.1") == 100000);

   /* OpenAI */
   assert(model_context_window("gpt-5.4") == 272000);
   assert(model_context_window("gpt-5.3-codex") == 272000);
   assert(model_context_window("gpt-5.2") == 272000);
   assert(model_context_window("gpt-4o") == 128000);
   assert(model_context_window("gpt-4-turbo") == 128000);
   assert(model_context_window("gpt-3.5-turbo") == 16384);
   assert(model_context_window("o1") == 200000);
   assert(model_context_window("o3-mini") == 200000);

   /* Gemini */
   assert(model_context_window("gemini-1.5-pro") == 1000000);
   assert(model_context_window("gemini-2.5-pro") == 1000000);

   /* Mistral */
   assert(model_context_window("mistral-large-latest") == 128000);
   assert(model_context_window("mistral-small-latest") == 128000);
   assert(model_context_window("codestral-latest") == 256000);
   assert(model_context_window("MiniMax-M2.7") == 200000);

   /* Unknown model returns 0 */
   assert(model_context_window("some-unknown-model") == 0);
   assert(model_context_window("") == 0);
   assert(model_context_window(NULL) == 0);

   /* Case insensitive */
   assert(model_context_window("Claude-Opus-4-6") == 200000);
   assert(model_context_window("GPT-4o") == 128000);
}

static void test_max_output(void)
{
   /* Static-table models return their pinned output ceiling. */
   assert(model_max_output("openai", "gpt-4o") == 16384);
   assert(model_max_output("anthropic", "claude-sonnet-4-6") == 8192);

   /* Inferred (heuristic) models: reasoning families get a higher ceiling than
    * plain chat, both well above the old hardcoded 4096 default. */
   assert(model_max_output("minimax", "MiniMax-M3") == 32768);      /* reasoning */
   assert(model_max_output(NULL, "mistral-medium-latest") == 8192); /* non-reasoning */

   /* Never starves a reasoning model at 4096 (the bug this replaced). */
   assert(model_max_output("minimax", "MiniMax-M3") > 4096);

   /* Inferred ceiling is clamped to the model's context window. */
   assert(model_max_output("openai", "gpt-3.5-turbo") <= model_context_window("gpt-3.5-turbo"));

   /* Unknown model still yields a usable, non-zero cap (never 0). */
   assert(model_max_output(NULL, "some-unknown-model") == 8192);
   assert(model_max_output(NULL, "") == 8192);
   assert(model_max_output(NULL, NULL) == 8192);
}

static void test_alias_list(void)
{
   int total = model_alias_list(NULL, 0);
   assert(total > 0);

   model_info_t buf[64];
   int n = model_alias_list(buf, 64);
   assert(n == total);

   /* Each entry should have non-empty provider and model_id */
   for (int i = 0; i < n && i < 64; i++)
   {
      assert(buf[i].provider[0] != '\0');
      assert(buf[i].model_id[0] != '\0');
   }

   /* Listing with max < total should return capped set */
   int capped = model_alias_list(buf, 3);
   assert(capped == total); /* returns total count */
}

static void test_model_capability_get(void)
{
   model_capability_t cap;

   assert(model_capability_get("openai", "gpt-4o", &cap) == 1);
   assert(cap.context_window == 128000);
   assert(cap.flags & MODEL_CAP_TOOLS);
   assert(cap.flags & MODEL_CAP_STREAMING);
   assert(cap.flags & MODEL_CAP_VISION);
   assert(strcmp(cap.provider, "openai") == 0);
   assert(strcmp(cap.model_id, "gpt-4o") == 0);
   assert(strcmp(cap.modalities, "text,image,audio") == 0);
   assert(cap.deprecated == 0);

   assert(model_capability_get("anthropic", "claude-opus-4-6", &cap) == 1);
   assert(cap.context_window == 200000);
   assert(cap.flags & MODEL_CAP_REASONING);
   assert(cap.flags & MODEL_CAP_PDF);

   assert(model_capability_get("gemini", "gemini-2.5-pro", &cap) == 1);
   assert(cap.context_window == 1000000);
   assert(cap.flags & MODEL_CAP_VISION);
   assert(cap.flags & MODEL_CAP_PDF);

   assert(model_capability_get(NULL, "codestral-latest", &cap) == 1);
   assert(cap.context_window == 256000);
   assert(cap.flags & MODEL_CAP_TOOLS);
   assert(strcmp(cap.modalities, "text") == 0);

   assert(model_capability_get("minimax", "MiniMax-M2.7", &cap) == 1);
   assert(cap.context_window == 200000);
   assert(cap.flags & MODEL_CAP_REASONING);
   assert(cap.flags & MODEL_CAP_TOOLS);
   assert(cap.flags & MODEL_CAP_STREAMING);
   assert(strcmp(cap.provider, "minimax") == 0);

   assert(model_capability_get("openai", "gpt-old-deprecated", &cap) == 1);
   assert(cap.deprecated == 1);

   assert(model_capability_get("openrouter", "anthropic/claude-opus-4.6", &cap) == 1);
   assert(cap.context_window == 200000);
   assert(cap.flags & MODEL_CAP_REASONING);
   assert(cap.flags & MODEL_CAP_VISION);

   assert(model_capability_get("openai", "", &cap) == 0);
   assert(model_capability_get("openai", "gpt-4o", NULL) == 0);
}

static void test_model_capability_helpers(void)
{
   model_capability_t cap;
   char provider[MODEL_PROVIDER_MAX];
   char model_id[MODEL_ID_MAX];
   assert(model_capability_resolve_ref("opus", provider, sizeof(provider), model_id,
                                       sizeof(model_id), &cap) == 1);
   assert(strcmp(provider, "anthropic") == 0);
   assert(strstr(model_id, "opus") != NULL);
   assert(cap.flags & MODEL_CAP_REASONING);

   assert(model_capability_resolve_ref("openrouter:anthropic/claude-opus-4.6", provider,
                                       sizeof(provider), model_id, sizeof(model_id), &cap) == 1);
   assert(strcmp(provider, "openrouter") == 0);
   assert(strcmp(model_id, "anthropic/claude-opus-4.6") == 0);
   assert(cap.context_window == 200000);
   assert(cap.flags & MODEL_CAP_PDF);

   assert(model_capability_resolve_ref(NULL, provider, sizeof(provider), model_id, sizeof(model_id),
                                       &cap) == 0);
   assert(model_capability_resolve_ref("", provider, sizeof(provider), model_id, sizeof(model_id),
                                       &cap) == 0);
   assert(model_capability_resolve_ref("openai:", provider, sizeof(provider), model_id,
                                       sizeof(model_id), &cap) == 0);
   assert(model_capability_resolve_ref("openai:gpt-4o", provider, 4, model_id, sizeof(model_id),
                                       &cap) == 0);
   assert(model_capability_resolve_ref("openai:gpt-4o", provider, sizeof(provider), model_id, 4,
                                       &cap) == 0);

   assert(model_capability_flag_from_name("vision") == MODEL_CAP_VISION);
   assert(model_capability_flag_from_name("image") == MODEL_CAP_VISION);
   assert(model_capability_flag_from_name("unknown") == 0);

   char flags[128];
   model_capability_format_flags(MODEL_CAP_TOOLS | MODEL_CAP_VISION, flags, sizeof(flags));
   assert(strstr(flags, "tools") != NULL);
   assert(strstr(flags, "vision") != NULL);
   assert(model_capability_get(NULL, "gpt4o", &cap) == 1);
   assert(strcmp(cap.provider, "openai") == 0);
   assert(strcmp(cap.model_id, "gpt-4o") == 0);

   assert(model_capability_get("openai", "gpt-4-turbo", &cap) == 1);
   assert(cap.deprecated == 1);

   model_capability_t buf[8];
   int total = model_capability_list(buf, 8, MODEL_CAP_VISION, 0);
   assert(total > 0);
   for (int i = 0; i < total && i < 8; i++)
      assert((buf[i].flags & MODEL_CAP_VISION) != 0);

   total = model_capability_list(buf, 8, 0, 1);
   assert(total > 0);
   for (int i = 0; i < total && i < 8; i++)
      assert(buf[i].open_weights == 1);

   model_capability_flags_string(MODEL_CAP_TOOLS | MODEL_CAP_PDF, flags, sizeof(flags));
   assert(strstr(flags, "tools") != NULL);
   assert(strstr(flags, "pdf") != NULL);
   assert(model_capability_flag_from_name("vision") == MODEL_CAP_VISION);
}

static void test_model_capability_refresh_cache_and_overrides(void)
{
   char tmpdir[] = "/tmp/aimee-model-registry-XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   char cache_home[512];
   snprintf(cache_home, sizeof(cache_home), "%s/cache", tmpdir);
   char cache_aimee[512];
   snprintf(cache_aimee, sizeof(cache_aimee), "%s/aimee", cache_home);
   assert(mkdir(cache_home, 0700) == 0);
   assert(mkdir(cache_aimee, 0700) == 0);

   char snapshot_path[512];
   snprintf(snapshot_path, sizeof(snapshot_path), "%s/models.json", tmpdir);
   FILE *fp = fopen(snapshot_path, "w");
   assert(fp != NULL);
   fputs("{\"models\":[{\"provider\":\"openai\","
         "\"model\":\"custom-vision\","
         "\"context_window\":65536,"
         "\"max_output\":2048,\"cost_in_per_mtok\":1.0,\"cost_out_per_mtok\":2.0,"
         "\"flags\":6,\"knowledge_cutoff\":\"2025-01\",\"open_weights\":0,"
         "\"deprecated\":0}]}\n",
         fp);
   fclose(fp);

   char override_path[512];
   snprintf(override_path, sizeof(override_path), "%s/override.json", tmpdir);
   fp = fopen(override_path, "w");
   assert(fp != NULL);
   fputs("{\"models\":[{\"provider\":\"openai\","
         "\"model\":\"custom-vision\","
         "\"context_window\":77777,"
         "\"max_output\":1024,\"cost_in_per_mtok\":0.5,\"cost_out_per_mtok\":1.5,"
         "\"flags\":7,\"knowledge_cutoff\":\"2026-01\",\"open_weights\":1,"
         "\"deprecated\":1}]}\n",
         fp);
   fclose(fp);

   char *old_cache = getenv("XDG_CACHE_HOME") ? strdup(getenv("XDG_CACHE_HOME")) : NULL;
   char *old_snapshot =
       getenv("AIMEE_MODELS_DEV_SNAPSHOT") ? strdup(getenv("AIMEE_MODELS_DEV_SNAPSHOT")) : NULL;
   char *old_override = getenv("AIMEE_MODEL_CAPABILITY_OVERRIDES")
                            ? strdup(getenv("AIMEE_MODEL_CAPABILITY_OVERRIDES"))
                            : NULL;

   setenv("XDG_CACHE_HOME", cache_home, 1);
   setenv("AIMEE_MODELS_DEV_SNAPSHOT", snapshot_path, 1);
   setenv("AIMEE_MODEL_CAPABILITY_OVERRIDES", override_path, 1);

   char msg[256];
   int refreshed = model_capability_refresh(msg, sizeof(msg));
   assert(refreshed > 0);

   model_capability_t cap;
   assert(model_capability_get("openai", "custom-vision", &cap) == 1);
   assert(cap.context_window == 77777);
   assert(cap.deprecated == 1);
   assert(cap.open_weights == 1);
   assert((cap.flags & MODEL_CAP_VISION) != 0);
   assert((cap.flags & MODEL_CAP_TOOLS) != 0);

   assert(unlink(snapshot_path) == 0);
   refreshed = model_capability_refresh(msg, sizeof(msg));
   assert(refreshed > 0);
   assert(model_capability_get("openai", "custom-vision", &cap) == 1);
   assert(cap.context_window == 77777);

   if (old_cache)
   {
      setenv("XDG_CACHE_HOME", old_cache, 1);
      free(old_cache);
   }
   else
      unsetenv("XDG_CACHE_HOME");

   if (old_snapshot)
   {
      setenv("AIMEE_MODELS_DEV_SNAPSHOT", old_snapshot, 1);
      free(old_snapshot);
   }
   else
      unsetenv("AIMEE_MODELS_DEV_SNAPSHOT");

   if (old_override)
   {
      setenv("AIMEE_MODEL_CAPABILITY_OVERRIDES", old_override, 1);
      free(old_override);
   }
   else
      unsetenv("AIMEE_MODEL_CAPABILITY_OVERRIDES");

   char cmd[640];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   assert(system(cmd) == 0);
}

static void test_models_dev_stub(void)
{
   model_capability_t caps;
   /* Stub always returns 0 (not found) */
   assert(models_dev_capability_get("anthropic", "claude-opus-4-6", &caps) == 0);
   assert(models_dev_capability_get("openai", "gpt-4o", &caps) == 0);
   assert(models_dev_capability_get(NULL, NULL, NULL) == 0);
}

int main(void)
{
   printf("model_registry: ");
   test_alias_resolve();
   printf("alias_resolve OK, ");
   test_provider_detect();
   printf("provider_detect OK, ");
   test_context_window();
   printf("context_window OK, ");
   test_max_output();
   printf("max_output OK, ");
   test_alias_list();
   printf("alias_list OK, ");
   test_model_capability_get();
   printf("capability OK, ");
   test_model_capability_helpers();
   printf("helpers OK, ");
   test_model_capability_refresh_cache_and_overrides();
   printf("refresh OK\n");
   test_models_dev_stub();
   printf("models_dev_stub OK\n");
   printf("models_dev_stub OK\n");
   return 0;
}
