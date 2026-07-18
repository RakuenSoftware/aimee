/* test_provider_catalog.c: unit tests for provider health/locality catalog */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../server/provider_catalog.c"

/* Stub the co-located /v1 dispatch with a minimal single-agent slot counter, so
 * provider_catalog_concurrent_{acquire,release} exercise their real
 * server-authoritative path (the production path on the server) end-to-end. */
static int g_stub_slots = 0;
cJSON *cli_v1_dispatch_local(cJSON *req, int timeout_ms)
{
   (void)timeout_ms;
   cJSON *mj = cJSON_GetObjectItemCaseSensitive(req, "method");
   const char *method = (mj && cJSON_IsString(mj)) ? mj->valuestring : "";
   cJSON *resp = cJSON_CreateObject();
   if (strcmp(method, "provider.slot_acquire") == 0)
   {
      cJSON *mp = cJSON_GetObjectItemCaseSensitive(req, "max_parallel");
      int max = (mp && cJSON_IsNumber(mp)) ? (int)mp->valuedouble : 0;
      int ok = (max <= 0) || (g_stub_slots < max);
      if (ok)
         g_stub_slots++;
      cJSON_AddBoolToObject(resp, "acquired", ok ? 1 : 0);
   }
   else if (strcmp(method, "provider.slot_release") == 0)
   {
      if (g_stub_slots > 0)
         g_stub_slots--;
   }
   return resp;
}

/* Reset module state between tests */
static void reset_catalog(void)
{
   pthread_mutex_lock(&g_cat.lock);
   memset(g_cat.entries, 0, sizeof(g_cat.entries));
   g_cat.count = 0;
   pthread_mutex_unlock(&g_cat.lock);
}

/* ---- locality classification ---- */

static void test_classify_loopback(void)
{
   assert(provider_catalog_classify_endpoint("http://localhost:11434") == PROVIDER_LOCALITY_LOCAL);
   assert(provider_catalog_classify_endpoint("http://127.0.0.1:8080") == PROVIDER_LOCALITY_LOCAL);
   assert(provider_catalog_classify_endpoint("http://[::1]:8080") == PROVIDER_LOCALITY_LOCAL);
   assert(provider_catalog_classify_endpoint("unix:///run/ollama.sock") == PROVIDER_LOCALITY_LOCAL);
   assert(provider_catalog_classify_endpoint("/run/ollama.sock") == PROVIDER_LOCALITY_LOCAL);
   assert(provider_catalog_classify_endpoint("http://0.0.0.0:5000") == PROVIDER_LOCALITY_LOCAL);
   printf("  classify_loopback: OK\n");
}

static void test_classify_lan(void)
{
   assert(provider_catalog_classify_endpoint("http://192.168.1.10:11434") == PROVIDER_LOCALITY_LAN);
   assert(provider_catalog_classify_endpoint("http://10.0.0.1:8080") == PROVIDER_LOCALITY_LAN);
   assert(provider_catalog_classify_endpoint("http://172.16.0.1:8080") == PROVIDER_LOCALITY_LAN);
   assert(provider_catalog_classify_endpoint("http://172.31.255.1:8080") == PROVIDER_LOCALITY_LAN);
   assert(provider_catalog_classify_endpoint("http://169.254.1.1:8080") == PROVIDER_LOCALITY_LAN);
   printf("  classify_lan: OK\n");
}

static void test_classify_remote(void)
{
   assert(provider_catalog_classify_endpoint("https://api.anthropic.com") ==
          PROVIDER_LOCALITY_REMOTE);
   assert(provider_catalog_classify_endpoint("https://api.openai.com/v1") ==
          PROVIDER_LOCALITY_REMOTE);
   assert(provider_catalog_classify_endpoint("http://172.32.0.1") == PROVIDER_LOCALITY_REMOTE);
   assert(provider_catalog_classify_endpoint("http://172.15.0.1") == PROVIDER_LOCALITY_REMOTE);
   printf("  classify_remote: OK\n");
}

static void test_classify_edge_cases(void)
{
   assert(provider_catalog_classify_endpoint(NULL) == PROVIDER_LOCALITY_UNKNOWN);
   assert(provider_catalog_classify_endpoint("") == PROVIDER_LOCALITY_UNKNOWN);
   printf("  classify_edge_cases: OK\n");
}

/* ---- health state transitions ---- */

static agent_t make_agent(const char *name, const char *endpoint)
{
   agent_t ag;
   memset(&ag, 0, sizeof(ag));
   snprintf(ag.name, sizeof(ag.name), "%s", name);
   snprintf(ag.endpoint, sizeof(ag.endpoint), "%s", endpoint);
   snprintf(ag.provider, sizeof(ag.provider), "test");
   ag.enabled = 1;
   return ag;
}

static void test_health_initial(void)
{
   reset_catalog();
   agent_t ag = make_agent("local-llm", "http://localhost:11434");
   provider_catalog_init(&ag, 1);
   assert(provider_catalog_get_health("local-llm") == CATALOG_HEALTH_HEALTHY);
   printf("  health_initial: OK\n");
}

static void test_health_degraded_after_one_failure(void)
{
   reset_catalog();
   agent_t ag = make_agent("local-llm", "http://localhost:11434");
   provider_catalog_init(&ag, 1);
   provider_catalog_record_failure("local-llm", "error");
   assert(provider_catalog_get_health("local-llm") == CATALOG_HEALTH_DEGRADED);
   printf("  health_degraded_after_one_failure: OK\n");
}

static void test_health_down_after_three_failures(void)
{
   reset_catalog();
   agent_t ag = make_agent("local-llm", "http://localhost:11434");
   provider_catalog_init(&ag, 1);
   provider_catalog_record_failure("local-llm", "retryable");
   provider_catalog_record_failure("local-llm", "retryable");
   provider_catalog_record_failure("local-llm", "retryable");
   assert(provider_catalog_get_health("local-llm") == CATALOG_HEALTH_DOWN);
   printf("  health_down_after_three_failures: OK\n");
}

static void test_health_recovers_on_success(void)
{
   reset_catalog();
   agent_t ag = make_agent("local-llm", "http://localhost:11434");
   provider_catalog_init(&ag, 1);
   provider_catalog_record_failure("local-llm", "error");
   provider_catalog_record_failure("local-llm", "error");
   assert(provider_catalog_get_health("local-llm") == CATALOG_HEALTH_DEGRADED);
   provider_catalog_record_success("local-llm");
   assert(provider_catalog_get_health("local-llm") == CATALOG_HEALTH_HEALTHY);
   printf("  health_recovers_on_success: OK\n");
}

/* Backdate an entry's last_failure so the cooldown is considered elapsed. */
static void backdate_last_failure(const char *name, int seconds_ago)
{
   pthread_mutex_lock(&g_cat.lock);
   provider_catalog_entry_t *e = find_entry_locked(name);
   if (e)
      e->last_failure = time(NULL) - seconds_ago;
   pthread_mutex_unlock(&g_cat.lock);
}

static void test_health_half_opens_after_cooldown(void)
{
   reset_catalog();
   agent_t ag = make_agent("flaky", "https://api.example.com");
   provider_catalog_init(&ag, 1);
   provider_catalog_record_failure("flaky", "retryable");
   provider_catalog_record_failure("flaky", "retryable");
   provider_catalog_record_failure("flaky", "retryable");
   assert(provider_catalog_get_health("flaky") == CATALOG_HEALTH_DOWN);

   /* Still DOWN within the cooldown window. */
   backdate_last_failure("flaky", PROVIDER_DOWN_COOLDOWN_SECONDS - 5);
   assert(provider_catalog_get_health("flaky") == CATALOG_HEALTH_DOWN);

   /* Half-opens (routable again) once the cooldown elapses. */
   backdate_last_failure("flaky", PROVIDER_DOWN_COOLDOWN_SECONDS + 5);
   assert(provider_catalog_get_health("flaky") == CATALOG_HEALTH_DEGRADED);

   /* A probe failure snaps it straight back to DOWN for another cooldown. */
   provider_catalog_record_failure("flaky", "retryable");
   assert(provider_catalog_get_health("flaky") == CATALOG_HEALTH_DOWN);

   /* A probe success clears the breaker entirely. */
   backdate_last_failure("flaky", PROVIDER_DOWN_COOLDOWN_SECONDS + 5);
   assert(provider_catalog_get_health("flaky") == CATALOG_HEALTH_DEGRADED);
   provider_catalog_record_success("flaky");
   assert(provider_catalog_get_health("flaky") == CATALOG_HEALTH_HEALTHY);
   printf("  health_half_opens_after_cooldown: OK\n");
}

static void test_health_unknown_agent_is_healthy(void)
{
   reset_catalog();
   assert(provider_catalog_get_health("nonexistent") == CATALOG_HEALTH_HEALTHY);
   assert(provider_catalog_get_health(NULL) == CATALOG_HEALTH_HEALTHY);
   printf("  health_unknown_agent_is_healthy: OK\n");
}

/* ---- locality queries ---- */

static void test_locality_local_agent(void)
{
   reset_catalog();
   agent_t ag = make_agent("ollama", "http://localhost:11434");
   provider_catalog_init(&ag, 1);
   assert(provider_catalog_get_locality("ollama") == PROVIDER_LOCALITY_LOCAL);
   printf("  locality_local_agent: OK\n");
}

static void test_locality_lan_agent(void)
{
   reset_catalog();
   agent_t ag = make_agent("lan-llm", "http://192.168.1.50:8080");
   provider_catalog_init(&ag, 1);
   assert(provider_catalog_get_locality("lan-llm") == PROVIDER_LOCALITY_LAN);
   printf("  locality_lan_agent: OK\n");
}

static void test_locality_unknown_agent(void)
{
   reset_catalog();
   assert(provider_catalog_get_locality("ghost") == PROVIDER_LOCALITY_UNKNOWN);
   assert(provider_catalog_get_locality(NULL) == PROVIDER_LOCALITY_UNKNOWN);
   printf("  locality_unknown_agent: OK\n");
}

/* ---- concurrency inference ---- */

static void test_inferred_concurrency_local(void)
{
   reset_catalog();
   agent_t ag = make_agent("ollama", "http://localhost:11434");
   provider_catalog_init(&ag, 1);
   assert(provider_catalog_inferred_concurrency("ollama") == PROVIDER_LOCAL_CONCURRENCY_DEFAULT);
   printf("  inferred_concurrency_local: OK\n");
}

static void test_inferred_concurrency_lan(void)
{
   reset_catalog();
   agent_t ag = make_agent("lan-llm", "http://10.0.0.5:8080");
   provider_catalog_init(&ag, 1);
   assert(provider_catalog_inferred_concurrency("lan-llm") == PROVIDER_LAN_CONCURRENCY_DEFAULT);
   printf("  inferred_concurrency_lan: OK\n");
}

static void test_inferred_concurrency_remote_is_zero(void)
{
   reset_catalog();
   agent_t ag = make_agent("claude", "https://api.anthropic.com");
   provider_catalog_init(&ag, 1);
   assert(provider_catalog_inferred_concurrency("claude") == 0);
   printf("  inferred_concurrency_remote_is_zero: OK\n");
}

/* ---- dump_json ---- */

static void test_dump_json_single_entry(void)
{
   reset_catalog();
   agent_t ag = make_agent("ollama", "http://localhost:11434");
   provider_catalog_init(&ag, 1);

   char buf[1024];
   int n = provider_catalog_dump_json(buf, sizeof(buf));
   assert(n > 0);
   assert(strstr(buf, "\"agent\":\"ollama\"") != NULL);
   assert(strstr(buf, "\"locality\":\"local\"") != NULL);
   assert(strstr(buf, "\"health\":\"healthy\"") != NULL);
   printf("  dump_json_single_entry: OK\n");
}

static void test_dump_json_overflow(void)
{
   reset_catalog();
   agent_t ag = make_agent("ollama", "http://localhost:11434");
   provider_catalog_init(&ag, 1);

   char tiny[5];
   int n = provider_catalog_dump_json(tiny, sizeof(tiny));
   assert(n == -1);
   printf("  dump_json_overflow: OK\n");
}

/* ---- label helpers ---- */

static void test_labels(void)
{
   assert(strcmp(provider_locality_label(PROVIDER_LOCALITY_LOCAL), "local") == 0);
   assert(strcmp(provider_locality_label(PROVIDER_LOCALITY_LAN), "lan") == 0);
   assert(strcmp(provider_locality_label(PROVIDER_LOCALITY_REMOTE), "remote") == 0);
   assert(strcmp(provider_locality_label(PROVIDER_LOCALITY_UNKNOWN), "unknown") == 0);
   assert(strcmp(catalog_health_label(CATALOG_HEALTH_HEALTHY), "healthy") == 0);
   assert(strcmp(catalog_health_label(CATALOG_HEALTH_DEGRADED), "degraded") == 0);
   assert(strcmp(catalog_health_label(CATALOG_HEALTH_STALE), "stale") == 0);
   assert(strcmp(catalog_health_label(CATALOG_HEALTH_DOWN), "down") == 0);
   printf("  labels: OK\n");
}

int main(void)
{
   printf("test_provider_catalog:\n");

   test_classify_loopback();
   test_classify_lan();
   test_classify_remote();
   test_classify_edge_cases();

   test_health_initial();
   test_health_degraded_after_one_failure();
   test_health_down_after_three_failures();
   test_health_recovers_on_success();
   test_health_half_opens_after_cooldown();
   test_health_unknown_agent_is_healthy();

   test_locality_local_agent();
   test_locality_lan_agent();
   test_locality_unknown_agent();

   test_inferred_concurrency_local();
   test_inferred_concurrency_lan();
   test_inferred_concurrency_remote_is_zero();

   test_dump_json_single_entry();
   test_dump_json_overflow();

   test_labels();


   printf("All provider_catalog tests passed.\n");
   return 0;
}
