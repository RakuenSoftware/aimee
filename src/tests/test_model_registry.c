#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aimee.h"
#include "model_registry.h"
#include "models_dev.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

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
   /* Anthropic. These previously asserted 200000 across the board, which is the
    * hand-written prefix table's value, not the model's: the 4.6 generation
    * carries a 1M window. The old assertion agreed with the stale table rather
    * than with the catalogue, so the suite stayed green while the fleet sized
    * every Claude request at a fifth of the real window. Assert against the
    * catalogue below (test_registry_agrees_with_catalog) rather than swapping
    * one hardcoded number for another. */
   assert(model_context_window("claude-opus-4-6") == 1000000);
   assert(model_context_window("claude-sonnet-4-6") == 1000000);
   assert(model_context_window("claude-haiku-4-5-20251001") == 200000);
   /* Not in the catalogue -- still served by the prefix-table fallback. */
   assert(model_context_window("claude-3-5-sonnet-20241022") == 200000);

   /* Claude 2: 100k */
   assert(model_context_window("claude-2.1") == 100000);

   /* OpenAI. The prefix table mapped the whole gpt-5.x family to 272000, which
    * is not a context window at all -- it is the threshold where gpt-5.6-sol's
    * price band steps up (see model_registry.h). The catalogue's real windows
    * are larger and differ per model. */
   assert(model_context_window("gpt-5.4") == 1050000);
   assert(model_context_window("gpt-5.3-codex") == 400000);
   assert(model_context_window("gpt-5.2") == 400000);
   assert(model_context_window("gpt-4o") == 128000);
   assert(model_context_window("gpt-4-turbo") == 128000);
   assert(model_context_window("gpt-3.5-turbo") == 16385);
   assert(model_context_window("o1") == 200000);
   assert(model_context_window("o3-mini") == 200000);

   /* Gemini. 1.5-pro is not in the catalogue, so the prefix table still serves
    * it; 2.5-pro is, and its real window is 1048576 rather than a round 1M. */
   assert(model_context_window("gemini-1.5-pro") == 1000000);
   assert(model_context_window("gemini-2.5-pro") == 1048576);

   /* Mistral / MiniMax -- all previously rounded down by the prefix table. */
   assert(model_context_window("mistral-large-latest") == 262144);
   assert(model_context_window("mistral-small-latest") == 256000);
   assert(model_context_window("codestral-latest") == 256000);
   assert(model_context_window("MiniMax-M2.7") == 204800);

   /* Unknown model returns 0 */
   assert(model_context_window("some-unknown-model") == 0);
   assert(model_context_window("") == 0);
   assert(model_context_window(NULL) == 0);

   /* Case insensitive */
   assert(model_context_window("Claude-Opus-4-6") == 1000000);
   assert(model_context_window("GPT-4o") == 128000);
}

static void test_max_output(void)
{
   /* Catalogued models return the ceiling the catalogue publishes. gpt-4o's
    * static row happened to match it (16384); claude-sonnet-4-6's did not --
    * the static table pinned 8192 against a real ceiling of 128000, capping
    * every Claude response at a sixteenth of what the model can emit. */
   assert(model_max_output("openai", "gpt-4o") == 16384);
   assert(model_max_output("anthropic", "claude-sonnet-4-6") == 128000);

   /* A CATALOGUED model returns the ceiling the catalog publishes, not an
    * inferred one. MiniMax-M3 and mistral-medium-latest used to sit here as
    * heuristic examples, which only held while the bundled snapshot was
    * unreachable; both are in the catalog, so they now answer from it. */
   assert(model_max_output("minimax", "MiniMax-M3") == 128000);

   /* Inferred (heuristic) models - names the catalog does NOT carry: reasoning
    * families get a higher ceiling than plain chat, both well above the old
    * hardcoded 4096 default. */
   assert(model_max_output("minimax", "MiniMax-R9-thinking-qqq") > 8192); /* reasoning */
   assert(model_max_output(NULL, "some-plain-chat-model-qqq") == 8192);   /* non-reasoning */

   /* Never starves a reasoning model at 4096 (the bug this replaced). */
   assert(model_max_output("minimax", "MiniMax-M3") > 4096);
   assert(model_max_output("minimax", "MiniMax-R9-thinking-qqq") > 4096);

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
   /* The static row claimed audio; the catalogue publishes text/image/pdf for
    * gpt-4o (audio input is a separate model family upstream), so the AUDIO flag
    * is now clear and the derived string drops to "text,image".
    * capability_set_modalities() emits only text / text,image / text,image,audio
    * -- it has no PDF form -- so the PDF flag is set on the record but absent
    * from this string. Modality caps are documented as best-effort routing
    * preferences that the router relaxes when they would disable every
    * candidate. */
   assert(strcmp(cap.modalities, "text,image") == 0);
   assert(cap.flags & MODEL_CAP_PDF);
   assert(cap.deprecated == 0);

   assert(model_capability_get("anthropic", "claude-opus-4-6", &cap) == 1);
   /* Was 200000/$15/$75 from the static table; the catalogue carries 1M and
    * $5/$25. The static row outranked the catalogue, so cost-based routing saw
    * Opus as 3x its real price. */
   assert(cap.context_window == 1000000);
   assert(cap.cost_in_per_mtok == 5.00);
   assert(cap.cost_out_per_mtok == 25.00);
   assert(cap.flags & MODEL_CAP_REASONING);
   assert(cap.flags & MODEL_CAP_PDF);

   assert(model_capability_get("gemini", "gemini-2.5-pro", &cap) == 1);
   assert(cap.context_window == 1048576); /* real window, not a rounded 1M */
   assert(cap.flags & MODEL_CAP_VISION);
   assert(cap.flags & MODEL_CAP_PDF);

   assert(model_capability_get(NULL, "codestral-latest", &cap) == 1);
   assert(cap.context_window == 256000);
   assert(cap.flags & MODEL_CAP_TOOLS);
   assert(strcmp(cap.modalities, "text") == 0);

   assert(model_capability_get("minimax", "MiniMax-M2.7", &cap) == 1);
   assert(cap.context_window == 204800);
   assert(cap.flags & MODEL_CAP_REASONING);
   assert(cap.flags & MODEL_CAP_TOOLS);
   assert(cap.flags & MODEL_CAP_STREAMING);
   assert(strcmp(cap.provider, "minimax") == 0);

   assert(model_capability_get("openai", "gpt-old-deprecated", &cap) == 1);
   assert(cap.deprecated == 1);

   /* An openrouter "vendor/model" id resolves through the catalog. The window is
    * asserted as a LOWER BOUND, not pinned: it comes from the bundled snapshot,
    * so pinning the exact number would fail the suite every time the snapshot is
    * regenerated and a vendor has moved it - the same trap
    * check-models-dev-snapshot.py deliberately avoids. (It pinned 200000, which
    * was the heuristic's answer back when the snapshot was unreachable.) */
   assert(model_capability_get("openrouter", "anthropic/claude-opus-4.6", &cap) == 1);
   assert(cap.context_window >= 200000);
   assert(cap.flags & MODEL_CAP_REASONING);
   assert(cap.flags & MODEL_CAP_VISION);
   assert(cap.flags & MODEL_CAP_PDF);

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
   /* Lower bound, not a pin - the window comes from the bundled snapshot. */
   assert(cap.context_window >= 200000);
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
   char tmpdir[256];
   snprintf(tmpdir, sizeof tmpdir, "%s/aimee-model-registry-XXXXXX", platform_tmpdir());
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

/* The invariant the previous tests could not express: where the catalogue
 * carries a model, the registry must REPORT the catalogue's numbers rather than
 * a hand-written approximation of them.
 *
 * This is deliberately written against the catalogue itself, not against
 * literals. A test that asserted `== 1000000` would be the same mistake one
 * release later: it would agree with whatever table someone typed, and go green
 * while the two drifted apart. Enumerating the catalogue is also what proves the
 * list walker works -- a zero-entry enumeration is exactly how this stayed
 * hidden, so an empty catalogue fails the test rather than vacuously passing. */
static void test_registry_agrees_with_catalog(void)
{
   static model_capability_t caps[1024];
   int n = models_dev_cache_list(caps, 1024, 0, 0);
   assert(n > 0 && "catalogue enumerated zero models -- the list walker is broken");
   if (n > 1024)
      n = 1024;

   int checked = 0;
   for (int i = 0; i < n; i++)
   {
      if (caps[i].context_window <= 0)
         continue;

      /* The window the rest of the fleet sizes requests with. */
      assert(model_context_window(caps[i].model_id) == caps[i].context_window);

      /* And the full capability record consumers route and price on. */
      model_capability_t got;
      memset(&got, 0, sizeof got);
      assert(model_capability_get(caps[i].provider, caps[i].model_id, &got) == 1);
      assert(got.context_window == caps[i].context_window);
      assert(got.max_output == caps[i].max_output);
      assert(got.cost_in_per_mtok == caps[i].cost_in_per_mtok);
      assert(got.cost_out_per_mtok == caps[i].cost_out_per_mtok);
      checked++;
   }
   assert(checked > 100 && "catalogue unexpectedly small; check the snapshot");
}

int main(void)
{
   printf("model_registry: ");
   /* First, before test_model_capability_refresh_cache_and_overrides() repoints
    * HOME/XDG_CACHE_HOME at a temp dir. This invariant describes a pristine
    * process reading the bundled snapshot; once the cache path is redirected,
    * the enumerator and the capability set can legitimately read different
    * files and disagreeing is not a defect. */
   test_registry_agrees_with_catalog();
   printf("registry_agrees_with_catalog OK, ");
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
   return 0;
}
