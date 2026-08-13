/* test_models_dev.c: tests for models.dev cache lookup */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aimee.h"
#include "models_dev.h"
#include "model_registry.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static void test_cache_lookup_hit(void)
{
   char tmpdir[256];
   snprintf(tmpdir, sizeof(tmpdir), "%s/test-models-dev-XXXXXX", platform_tmpdir());
   assert(mkdtemp(tmpdir) != NULL);

   char cache_parent[512], cache_dir[512], cache_path[512];
   snprintf(cache_parent, sizeof(cache_parent), "%s/.cache", tmpdir);
   snprintf(cache_dir, sizeof(cache_dir), "%s/.cache/aimee", tmpdir);
   snprintf(cache_path, sizeof(cache_path), "%s/models_dev.json", cache_dir);
   mkdir(cache_parent, 0755);
   mkdir(cache_dir, 0755);

   const char *json =
       "{\"anthropic/claude-test\": {\"contextWindow\": 200000, \"maxTokens\": 4096,"
       " \"inputCost\": 3.0, \"outputCost\": 15.0, \"tools\": true, \"vision\": false}}";
   FILE *f = fopen(cache_path, "w");
   assert(f);
   fputs(json, f);
   fclose(f);

   setenv("HOME", tmpdir, 1);

   model_capability_t caps;
   memset(&caps, 0, sizeof(caps));
   int rc = models_dev_cache_lookup("anthropic", "claude-test", &caps);
   assert(rc == 1);
   assert(caps.context_window == 200000);
   assert(caps.max_output == 4096);
   assert(caps.cost_in_per_mtok == 3.0);
   assert(caps.flags & MODEL_CAP_TOOLS);
   assert(!(caps.flags & MODEL_CAP_VISION));

   unlink(cache_path);
   rmdir(cache_dir);
   rmdir(cache_parent);
   rmdir(tmpdir);
}

/* The LIVE https://models.dev/api.json schema is NESTED, and models_dev_refresh()
 * curls it into the cache verbatim with no transform. Before the reader learned
 * this shape the downloaded cache resolved NOTHING — the flat "provider/model"
 * key lookup returned NULL against a nested root — so every capability fell
 * through to the heuristic and every price stayed 0. Uses the real field names
 * and real values for two live fleet models. */
static void test_cache_lookup_nested_api_schema(void)
{
   char tmpdir[256];
   snprintf(tmpdir, sizeof(tmpdir), "%s/test-models-dev-nested-XXXXXX", platform_tmpdir());
   assert(mkdtemp(tmpdir) != NULL);

   char cache_parent[512], cache_dir[512], cache_path[512];
   snprintf(cache_parent, sizeof(cache_parent), "%s/.cache", tmpdir);
   snprintf(cache_dir, sizeof(cache_dir), "%s/.cache/aimee", tmpdir);
   snprintf(cache_path, sizeof(cache_path), "%s/models_dev.json", cache_dir);
   mkdir(cache_parent, 0755);
   mkdir(cache_dir, 0755);

   const char *json = "{\"minimax\": {\"name\": \"MiniMax\", \"models\": {"
                      "  \"MiniMax-M3\": {\"name\": \"MiniMax M3\","
                      "    \"limit\": {\"context\": 1000000, \"output\": 128000},"
                      "    \"cost\": {\"input\": 0.3, \"output\": 1.2},"
                      "    \"tool_call\": true, \"reasoning\": true,"
                      "    \"modalities\": {\"input\": [\"text\", \"image\"]}}}},"
                      " \"openai\": {\"models\": {"
                      "  \"gpt-5.6-sol\": {\"name\": \"GPT-5.6 Sol\","
                      "    \"limit\": {\"context\": 1050000, \"output\": 128000},"
                      "    \"cost\": {\"input\": 5.0, \"output\": 30.0},"
                      "    \"tool_call\": true, \"reasoning\": true}}}}";
   FILE *f = fopen(cache_path, "w");
   assert(f);
   fputs(json, f);
   fclose(f);

   setenv("HOME", tmpdir, 1);

   model_capability_t caps;
   memset(&caps, 0, sizeof(caps));
   assert(models_dev_cache_lookup("minimax", "MiniMax-M3", &caps) == 1);
   assert(caps.context_window == 1000000);
   assert(caps.max_output == 128000);
   assert(caps.cost_in_per_mtok == 0.3);
   assert(caps.cost_out_per_mtok == 1.2);
   assert(caps.flags & MODEL_CAP_TOOLS);
   /* REASONING has no flat-schema equivalent; the nested reader is the only
    * source of it, and it is what selects the long per-call timeout. */
   assert(caps.flags & MODEL_CAP_REASONING);
   assert(caps.flags & MODEL_CAP_VISION);
   assert(strcmp(caps.display_name, "MiniMax M3") == 0);
   assert(!caps.deprecated);

   /* Price is the whole point of reading this schema: it is the only source of
    * a real cost basis, and it is 0 for every model without it. */
   memset(&caps, 0, sizeof(caps));
   assert(models_dev_cache_lookup("openai", "gpt-5.6-sol", &caps) == 1);
   assert(caps.cost_in_per_mtok == 5.0);
   assert(caps.cost_out_per_mtok == 30.0);
   assert(strcmp(caps.display_name, "GPT-5.6 Sol") == 0);

   /* A provider present but a model absent must still miss cleanly. */
   memset(&caps, 0, sizeof(caps));
   assert(models_dev_cache_lookup("minimax", "MiniMax-M99", &caps) == 0);

   unlink(cache_path);
   rmdir(cache_dir);
   rmdir(cache_parent);
   rmdir(tmpdir);
}

/* Band-schedule edge cases introduced by the capacity/eviction logic. Review
 * found duplicate handling at capacity silently losing data, and none of the
 * overflow paths were covered. */
static void test_price_band_capacity_and_duplicates(void)
{
   char tmpdir[256];
   snprintf(tmpdir, sizeof(tmpdir), "%s/test-md-bands-XXXXXX", platform_tmpdir());
   assert(mkdtemp(tmpdir) != NULL);
   char dir[512], path[600];
   snprintf(dir, sizeof(dir), "%s/.cache", tmpdir);
   mkdir(dir, 0755);
   snprintf(dir, sizeof(dir), "%s/.cache/aimee", tmpdir);
   mkdir(dir, 0755);
   snprintf(path, sizeof(path), "%s/models_dev.json", dir);

   /* Ten DESCENDING thresholds against a capacity of eight, then a duplicate of
    * a middle one and a duplicate of the largest retained. Descending order is
    * the case that a naive "keep the first N" would corrupt by discarding the
    * LOWEST bands, leaving mid-size requests on the base rate. */
   const char *json =
       "{\"v\":{\"models\":{\"m\":{"
       "  \"limit\":{\"context\":2000000},"
       "  \"cost\":{\"input\":1.0,\"output\":2.0,\"tiers\":["
       "    {\"input\":100,\"output\":100,\"tier\":{\"type\":\"context\",\"size\":1000}},"
       "    {\"input\":90,\"output\":90,\"tier\":{\"type\":\"context\",\"size\":900}},"
       "    {\"input\":80,\"output\":80,\"tier\":{\"type\":\"context\",\"size\":800}},"
       "    {\"input\":70,\"output\":70,\"tier\":{\"type\":\"context\",\"size\":700}},"
       "    {\"input\":60,\"output\":60,\"tier\":{\"type\":\"context\",\"size\":600}},"
       "    {\"input\":50,\"output\":50,\"tier\":{\"type\":\"context\",\"size\":500}},"
       "    {\"input\":40,\"output\":40,\"tier\":{\"type\":\"context\",\"size\":400}},"
       "    {\"input\":30,\"output\":30,\"tier\":{\"type\":\"context\",\"size\":300}},"
       "    {\"input\":20,\"output\":20,\"tier\":{\"type\":\"context\",\"size\":200}},"
       "    {\"input\":10,\"output\":10,\"tier\":{\"type\":\"context\",\"size\":100}},"
       /* duplicate of a MIDDLE retained threshold: must overwrite in place */
       "    {\"input\":11,\"output\":11,\"tier\":{\"type\":\"context\",\"size\":200}},"
       /* duplicate of the LARGEST retained threshold (800): must also overwrite,
        * not be discarded by the "higher than everything kept" eviction test */
       "    {\"input\":81,\"output\":81,\"tier\":{\"type\":\"context\",\"size\":800}},"
       /* non-context tier type: skipped rather than guessed at */
       "    {\"input\":77,\"output\":77,\"tier\":{\"type\":\"tokens\",\"size\":50}}"
       "  ]}}}}}";
   FILE *f = fopen(path, "w");
   assert(f);
   fputs(json, f);
   fclose(f);
   setenv("HOME", tmpdir, 1);

   model_capability_t c;
   memset(&c, 0, sizeof(c));
   assert(models_dev_cache_lookup("v", "m", &c) == 1);

   assert(c.price_band_count == MODEL_PRICE_BANDS_MAX);
   assert(c.price_bands_truncated == 1); /* ten distinct > capacity of eight */

   /* Ascending, and the LOWEST thresholds were the ones kept. */
   for (int i = 1; i < c.price_band_count; i++)
      assert(c.price_bands[i].above_tokens > c.price_bands[i - 1].above_tokens);
   assert(c.price_bands[0].above_tokens == 100);
   assert(c.price_bands[MODEL_PRICE_BANDS_MAX - 1].above_tokens == 800);

   /* Both duplicates overwrote IN PLACE — the middle one (200) and the largest
    * retained (800), the latter being the case the eviction branch would
    * otherwise discard — and neither evicted a distinct band to make room. */
   int seen_200 = 0, seen_800 = 0;
   for (int i = 0; i < c.price_band_count; i++)
   {
      if (c.price_bands[i].above_tokens == 200)
      {
         assert(c.price_bands[i].in_per_mtok == 11.0);
         seen_200 = 1;
      }
      if (c.price_bands[i].above_tokens == 800)
      {
         assert(c.price_bands[i].in_per_mtok == 81.0);
         seen_800 = 1;
      }
      /* The non-context tier type was skipped rather than guessed at. */
      assert(c.price_bands[i].above_tokens != 50);
   }
   assert(seen_200 && seen_800);

   unlink(path);
   rmdir(dir);
   snprintf(dir, sizeof(dir), "%s/.cache", tmpdir);
   rmdir(dir);
   rmdir(tmpdir);
}

/* Two claims the descending-overflow fixture CANNOT prove, because its array is
 * already full and already truncated before those cases arise:
 *   - a duplicate arriving at exactly capacity must not spuriously set
 *     price_bands_truncated;
 *   - INT_MAX must be rejected by the PARSER, not merely evicted by the
 *     capacity rule (which would discard it either way).
 * Both need free capacity or an exactly-full array, so they get their own
 * fixture rather than a passing-for-the-wrong-reason assertion. */
static void test_price_band_exact_capacity_and_intmax(void)
{
   char tmpdir[256];
   snprintf(tmpdir, sizeof(tmpdir), "%s/test-md-bands2-XXXXXX", platform_tmpdir());
   assert(mkdtemp(tmpdir) != NULL);
   char dir[512], path[600];
   snprintf(dir, sizeof(dir), "%s/.cache", tmpdir);
   mkdir(dir, 0755);
   snprintf(dir, sizeof(dir), "%s/.cache/aimee", tmpdir);
   mkdir(dir, 0755);
   snprintf(path, sizeof(path), "%s/models_dev.json", dir);

   /* Exactly MODEL_PRICE_BANDS_MAX (8) distinct thresholds, then a duplicate. */
   const char *json =
       "{\"v\":{\"models\":{\"m\":{"
       "  \"limit\":{\"context\":2000000},"
       "  \"cost\":{\"input\":1.0,\"output\":2.0,\"tiers\":["
       "    {\"input\":1,\"output\":1,\"tier\":{\"type\":\"context\",\"size\":100}},"
       "    {\"input\":2,\"output\":2,\"tier\":{\"type\":\"context\",\"size\":200}},"
       "    {\"input\":3,\"output\":3,\"tier\":{\"type\":\"context\",\"size\":300}},"
       "    {\"input\":4,\"output\":4,\"tier\":{\"type\":\"context\",\"size\":400}},"
       "    {\"input\":5,\"output\":5,\"tier\":{\"type\":\"context\",\"size\":500}},"
       "    {\"input\":6,\"output\":6,\"tier\":{\"type\":\"context\",\"size\":600}},"
       "    {\"input\":7,\"output\":7,\"tier\":{\"type\":\"context\",\"size\":700}},"
       "    {\"input\":8,\"output\":8,\"tier\":{\"type\":\"context\",\"size\":800}},"
       "    {\"input\":88,\"output\":88,\"tier\":{\"type\":\"context\",\"size\":800}}"
       "  ]}}}}}";
   FILE *f = fopen(path, "w");
   assert(f);
   fputs(json, f);
   fclose(f);
   setenv("HOME", tmpdir, 1);

   model_capability_t c;
   memset(&c, 0, sizeof(c));
   assert(models_dev_cache_lookup("v", "m", &c) == 1);
   assert(c.price_band_count == MODEL_PRICE_BANDS_MAX);
   /* The duplicate consumed no slot, so nothing was dropped: NOT truncated. */
   assert(c.price_bands_truncated == 0);
   /* ...and it overwrote the largest retained threshold in place. */
   assert(c.price_bands[MODEL_PRICE_BANDS_MAX - 1].above_tokens == 800);
   assert(c.price_bands[MODEL_PRICE_BANDS_MAX - 1].in_per_mtok == 88.0);

   /* INT_MAX with FREE capacity: only parser rejection can keep it out now. */
   const char *json2 = "{\"v\":{\"models\":{\"m\":{"
                       "  \"limit\":{\"context\":2000000},"
                       "  \"cost\":{\"input\":1.0,\"output\":2.0,\"tiers\":["
                       "    {\"input\":9,\"output\":9,"
                       "     \"tier\":{\"type\":\"context\",\"size\":2147483647}},"
                       "    {\"input\":5,\"output\":5,\"tier\":{\"type\":\"context\",\"size\":500}}"
                       "  ]}}}}}";
   f = fopen(path, "w");
   assert(f);
   fputs(json2, f);
   fclose(f);

   memset(&c, 0, sizeof(c));
   assert(models_dev_cache_lookup("v", "m", &c) == 1);
   assert(c.price_band_count == 1); /* only the 500 band survived */
   assert(c.price_bands[0].above_tokens == 500);
   assert(c.price_bands_truncated == 0);

   unlink(path);
   rmdir(dir);
   snprintf(dir, sizeof(dir), "%s/.cache", tmpdir);
   rmdir(dir);
   rmdir(tmpdir);
}

static void test_cache_lookup_miss(void)
{
   model_capability_t caps;
   memset(&caps, 0, sizeof(caps));
   int rc = models_dev_cache_lookup("unknown", "nonexistent-model", &caps);
   assert(rc == 0);
}

static void test_cache_lookup_null_guard(void)
{
   assert(models_dev_cache_lookup(NULL, "model", NULL) == 0);
   assert(models_dev_cache_lookup("prov", NULL, NULL) == 0);
}

static void test_stub_returns_zero(void)
{
   model_capability_t caps;
   int rc = models_dev_capability_get("anthropic", "claude-opus-4-6", &caps);
   assert(rc == 0);
}

int main(void)
{
   printf("models_dev: ");
   test_cache_lookup_hit();
   printf("cache_hit OK, ");
   test_cache_lookup_nested_api_schema();
   printf("nested_api OK, ");
   test_price_band_capacity_and_duplicates();
   printf("bands OK, ");
   test_price_band_exact_capacity_and_intmax();
   printf("bands_cap OK, ");
   test_cache_lookup_miss();
   printf("cache_miss OK, ");
   test_cache_lookup_null_guard();
   printf("null_guard OK, ");
   test_stub_returns_zero();
   printf("stub OK\n");
   return 0;
}
