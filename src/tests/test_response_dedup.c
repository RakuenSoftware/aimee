/* test_response_dedup.c: unit tests for the §4 short-window dedup cache. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "response_dedup.h"

#define PASS(name) printf("  %s: ok\n", name)

static void test_key_isolation(void)
{
   char k1[256], k2[256], k3[256], k4[256];
   response_dedup_key("uid:1", "gpt-4o", "/v1/chat/completions", "idem-a", "{\"x\":1}", "ctx", k1,
                      sizeof(k1));
   /* Different principal -> different key (no cross-account reads). */
   response_dedup_key("uid:2", "gpt-4o", "/v1/chat/completions", "idem-a", "{\"x\":1}", "ctx", k2,
                      sizeof(k2));
   /* Different body -> different key. */
   response_dedup_key("uid:1", "gpt-4o", "/v1/chat/completions", "idem-a", "{\"x\":2}", "ctx", k3,
                      sizeof(k3));
   /* Different pre-injected context, identical body -> different key (no stale
    * replay after the injected memory/context moved). */
   response_dedup_key("uid:1", "gpt-4o", "/v1/chat/completions", "idem-a", "{\"x\":1}", "ctx-NEW",
                      k4, sizeof(k4));
   assert(strcmp(k1, k2) != 0);
   assert(strcmp(k1, k3) != 0);
   assert(strcmp(k1, k4) != 0);

   /* Identical inputs -> identical key (deterministic). */
   char k1b[256];
   response_dedup_key("uid:1", "gpt-4o", "/v1/chat/completions", "idem-a", "{\"x\":1}", "ctx", k1b,
                      sizeof(k1b));
   assert(strcmp(k1, k1b) == 0);

   /* Empty principal collapses to a stable "anon" marker, not an empty field. */
   char ka[256];
   response_dedup_key("", "gpt-4o", "/v1/chat/completions", "idem-a", "{}", "", ka, sizeof(ka));
   assert(strncmp(ka, "anon|", 5) == 0);
   PASS("dedup: key isolation + determinism + context");
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
   test_key_isolation();
   test_get_put_roundtrip();
   test_ttl_expiry();
   test_empty_inputs_ignored();
   test_bounded_eviction();
   response_dedup_clear();
   printf("All response_dedup tests passed.\n");
   return 0;
}
