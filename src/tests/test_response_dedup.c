/* test_response_dedup.c: unit tests for the §4 short-window dedup cache. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "response_dedup.h"

#define PASS(name) printf("  %s: ok\n", name)

/* The response-composition module process derives the key (its Go tests hold
 * the digest, the field isolation and the anonymous marker); response_dedup only
 * requires that some provider answers. This stand-in answers deterministically. */
static int fixed_key_provider(const response_dedup_key_inputs_t *in, char *out, size_t out_cap)
{
   int n = snprintf(out, out_cap, "%s|fixed",
                    in->principal && in->principal[0] ? in->principal : "anon");
   return n > 0 && (size_t)n < out_cap ? 0 : -1;
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

   response_dedup_register_key_provider(fixed_key_provider);
   assert(response_dedup_key(&in, key, sizeof(key)) == 0);
   assert(strcmp(key, "uid:1|fixed") == 0);
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
   response_dedup_register_key_provider(fixed_key_provider);
   test_key_provider_required();
   test_get_put_roundtrip();
   test_ttl_expiry();
   test_empty_inputs_ignored();
   test_bounded_eviction();
   response_dedup_clear();
   printf("All response_dedup tests passed.\n");
   return 0;
}
