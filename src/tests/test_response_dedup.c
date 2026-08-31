/* test_response_dedup.c: unit tests for the §4 short-window dedup cache. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "response_dedup.h"
#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/response-composition/module_api.h>

#define PASS(name) printf("  %s: ok\n", name)

extern aimee_module_status_t aimee_module_handler(const aimee_module_invocation_t *invocation,
                                                  const uint8_t *request_body, uint32_t request_len,
                                                  uint8_t *response_body,
                                                  uint32_t response_capacity,
                                                  uint32_t *response_len, void *user_data);

int aimee_module_invocation_cancelled(const aimee_module_invocation_t *invocation)
{
   (void)invocation;
   return 0;
}

static int module_key_provider(const response_dedup_key_inputs_t *in, char *out, size_t out_cap)
{
   aimee_response_key_input_t module_input = {.principal = in->principal,
                                              .source = in->source,
                                              .provider = in->provider,
                                              .model = in->model,
                                              .endpoint = in->endpoint,
                                              .idempotency_key = in->idempotency_key,
                                              .body = in->body,
                                              .context = in->context,
                                              .behavior_flags = in->behavior_flags,
                                              .stream = in->stream};
   size_t request_len = aimee_response_request_size(&module_input);
   uint8_t *request = malloc(request_len);
   uint8_t response[AIMEE_RESPONSE_KEY_MAX + 4u];
   uint32_t response_len = 0;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_RESPONSE_STAGE_COMPOSE};
   if (!request || aimee_response_request_encode(&module_input, request, request_len) != 0)
   {
      free(request);
      return -1;
   }
   aimee_module_status_t status =
       aimee_module_handler(&invocation, request, (uint32_t)request_len, response, sizeof(response),
                            &response_len, NULL);
   free(request);
   return status == AIMEE_MODULE_STATUS_OK
              ? aimee_response_response_decode(response, response_len, out, out_cap)
              : -1;
}

static int failing_key_provider(const response_dedup_key_inputs_t *in, char *out, size_t out_cap)
{
   (void)in;
   if (out && out_cap > 0)
      out[0] = '\0';
   return -1;
}

static int empty_key_provider(const response_dedup_key_inputs_t *in, char *out, size_t out_cap)
{
   (void)in;
   if (out && out_cap > 0)
      out[0] = '\0';
   return 0;
}

static void test_key_isolation(void)
{
   char k1[256], k2[256], k3[256], k4[256], k5[256], k6[256], k7[256];
   /* Baseline. */
   response_dedup_key_inputs_t base = {.principal = "uid:1",
                                       .source = "openai-ingress",
                                       .provider = "openai",
                                       .model = "gpt-4o",
                                       .endpoint = "/v1/chat/completions",
                                       .stream = 0,
                                       .idempotency_key = "idem-a",
                                       .body = "{\"x\":1}",
                                       .context = "ctx",
                                       .behavior_flags = "cs0 rc0"};
   assert(response_dedup_key(&base, k1, sizeof(k1)) == 0);
   assert(strcmp(k1, "uid:1|45fd46a03cb4a28da3227155fec20a71") == 0);

   /* Different principal -> different key (no cross-account reads). */
   response_dedup_key_inputs_t v = base;
   v.principal = "uid:2";
   assert(response_dedup_key(&v, k2, sizeof(k2)) == 0);
   /* Different body. */
   v = base;
   v.body = "{\"x\":2}";
   assert(response_dedup_key(&v, k3, sizeof(k3)) == 0);
   /* Different pre-injected context, identical body. */
   v = base;
   v.context = "ctx-NEW";
   assert(response_dedup_key(&v, k4, sizeof(k4)) == 0);
   /* Different RESOLVED model — same requested-but-resolved-different must not
    * collide (the core finding-2 fix). */
   v = base;
   v.model = "gpt-4o-mini";
   assert(response_dedup_key(&v, k5, sizeof(k5)) == 0);
   /* Different resolved provider. */
   v = base;
   v.provider = "azure";
   assert(response_dedup_key(&v, k6, sizeof(k6)) == 0);
   /* Different behaviour-config flags. */
   v = base;
   v.behavior_flags = "cs1 rc0";
   assert(response_dedup_key(&v, k7, sizeof(k7)) == 0);

   assert(strcmp(k1, k2) != 0);
   assert(strcmp(k1, k3) != 0);
   assert(strcmp(k1, k4) != 0);
   assert(strcmp(k1, k5) != 0);
   assert(strcmp(k1, k6) != 0);
   assert(strcmp(k1, k7) != 0);

   /* Identical inputs -> identical key (deterministic). */
   char k1b[256];
   assert(response_dedup_key(&base, k1b, sizeof(k1b)) == 0);
   assert(strcmp(k1, k1b) == 0);

   /* Empty principal collapses to a stable "anon" marker, not an empty field. */
   response_dedup_key_inputs_t anon = base;
   anon.principal = "";
   char ka[256];
   assert(response_dedup_key(&anon, ka, sizeof(ka)) == 0);
   assert(strncmp(ka, "anon|", 5) == 0);

   /* Invalid inputs fail and leave no usable key. */
   char kn[8] = "x";
   assert(response_dedup_key(NULL, kn, sizeof(kn)) == -1);
   assert(kn[0] == '\0');
   PASS("dedup: key isolation incl. resolved backend + flags");
}

static void test_key_provider_required(void)
{
   response_dedup_key_inputs_t in = {.principal = "uid:1", .idempotency_key = "idem-a"};
   char key[64] = "stale";

   response_dedup_register_key_provider(NULL);
   assert(response_dedup_key(&in, key, sizeof(key)) == -1);
   assert(key[0] == '\0');

   response_dedup_register_key_provider(failing_key_provider);
   strcpy(key, "stale");
   assert(response_dedup_key(&in, key, sizeof(key)) == -1);
   assert(key[0] == '\0');

   response_dedup_register_key_provider(empty_key_provider);
   strcpy(key, "stale");
   assert(response_dedup_key(&in, key, sizeof(key)) == -1);
   assert(key[0] == '\0');

   response_dedup_register_key_provider(module_key_provider);
   PASS("dedup: event-bus key provider is required");
}

static void test_get_put_roundtrip(void)
{
   response_dedup_clear();
   char *out = NULL;
   double cost = -1.0;
   /* Miss before put. */
   assert(response_dedup_get("k1", 1000, &out, &cost) == 0);

   response_dedup_put("k1", "RESPONSE-BODY", 0.42, 1000, 5);
   assert(response_dedup_get("k1", 1002, &out, &cost) == 1);
   assert(out && strcmp(out, "RESPONSE-BODY") == 0);
   assert(cost > 0.41 && cost < 0.43);
   free(out);
   out = NULL;
   PASS("dedup: get/put roundtrip");
}

static void test_ttl_expiry(void)
{
   response_dedup_clear();
   char *out = NULL;
   response_dedup_put("k2", "BODY", 0.1, 1000, 5); /* expires at 1005 */
   assert(response_dedup_get("k2", 1004, &out, NULL) == 1);
   free(out);
   out = NULL;
   /* At/after expiry -> miss. */
   assert(response_dedup_get("k2", 1005, &out, NULL) == 0);
   assert(response_dedup_get("k2", 1010, &out, NULL) == 0);
   PASS("dedup: TTL expiry");
}

static void test_empty_inputs_ignored(void)
{
   response_dedup_clear();
   char *out = NULL;
   response_dedup_put("", "BODY", 0.1, 1000, 5); /* empty key ignored */
   response_dedup_put("k3", "", 0.1, 1000, 5);   /* empty body ignored */
   assert(response_dedup_get("", 1000, &out, NULL) == 0);
   assert(response_dedup_get("k3", 1000, &out, NULL) == 0);
   PASS("dedup: empty inputs ignored");
}

static void test_long_inputs_bounded_and_hit(void)
{
   /* A long idempotency key / model (the fields that used to be embedded
    * verbatim and could overflow the 192-byte slot) must still produce a BOUNDED
    * key, so an eligible retry actually hits (regression for the
    * truncated-key-never-hits bug). The principal stays verbatim but is bounded by
    * request_context (<=127); here we use a realistic-length one. */
   char principal[100], long_idem[300], long_model[200];
   memset(principal, 'P', sizeof(principal) - 1);
   principal[sizeof(principal) - 1] = '\0';
   memset(long_idem, 'K', sizeof(long_idem) - 1);
   long_idem[sizeof(long_idem) - 1] = '\0';
   memset(long_model, 'M', sizeof(long_model) - 1);
   long_model[sizeof(long_model) - 1] = '\0';

   response_dedup_key_inputs_t in = {.principal = principal,
                                     .source = "openai-ingress",
                                     .provider = "openai",
                                     .model = long_model,
                                     .endpoint = "/v1/chat/completions",
                                     .idempotency_key = long_idem,
                                     .body = "{\"x\":1}",
                                     .context = "ctx",
                                     .behavior_flags = "cs0 rc0"};
   char key[512];
   assert(response_dedup_key(&in, key, sizeof(key)) == 0);
   /* The composed key fits a cache slot (well under 192 bytes) despite the long
    * inputs, because the discriminators are digested to a fixed width. */
   assert(strlen(key) < 192);

   response_dedup_clear();
   response_dedup_put(key, "CACHED-LONG", 0.5, 1000, 5);
   char *out = NULL;
   /* Re-derive the key from identical inputs and confirm it HITS (it would miss
    * if either the stored or lookup key were silently truncated). */
   char key2[512];
   assert(response_dedup_key(&in, key2, sizeof(key2)) == 0);
   assert(strcmp(key, key2) == 0);
   assert(response_dedup_get(key2, 1001, &out, NULL) == 1);
   assert(out && strcmp(out, "CACHED-LONG") == 0);
   free(out);
   PASS("dedup: long inputs stay bounded and still hit");
}

static void test_bounded_eviction(void)
{
   response_dedup_clear();
   /* Insert far more entries than the slot count; the map stays bounded (no
    * crash / unbounded growth) and recent entries remain retrievable. */
   for (int i = 0; i < 500; i++)
   {
      char key[32], val[32];
      snprintf(key, sizeof(key), "key-%d", i);
      snprintf(val, sizeof(val), "val-%d", i);
      response_dedup_put(key, val, 0.01, 1000, 60);
   }
   /* The most recently inserted key should still be present. */
   char *out = NULL;
   assert(response_dedup_get("key-499", 1001, &out, NULL) == 1);
   assert(out && strcmp(out, "val-499") == 0);
   free(out);
   PASS("dedup: bounded eviction");
}

int main(void)
{
   printf("response_dedup: unit tests\n");
   response_dedup_register_key_provider(module_key_provider);
   test_key_provider_required();
   test_key_isolation();
   test_get_put_roundtrip();
   test_ttl_expiry();
   test_empty_inputs_ignored();
   test_long_inputs_bounded_and_hit();
   test_bounded_eviction();
   response_dedup_clear();
   printf("All response_dedup tests passed.\n");
   return 0;
}
