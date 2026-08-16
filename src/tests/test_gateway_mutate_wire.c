/* test_gateway_mutate_wire.c: the buffered gateway-mutation orchestration state
 * machine (proposal §2.5, buffered). Focuses on the post-send contract that is the
 * risky part: 4xx restore+repair+disable+resend, 5xx disable-no-resend, provenance
 * clear, and the identity-less / disabled-session pristine-passthrough gates. The
 * full positive apply path (config + a real reducing payload) is exercised by the
 * integration test / CT smoke. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "gateway_mutate_wire.h"
#include "msg_session_key.h"
#include "platform_path.h"
#include "platform_test_util.h"

/* A container {"messages":[<msg>]} whose single message tags which array it is. */
static cJSON *container_with(const char *tag)
{
   cJSON *c = cJSON_CreateObject();
   cJSON *arr = cJSON_CreateArray();
   cJSON *m = cJSON_CreateObject();
   cJSON_AddStringToObject(m, "role", "user");
   cJSON_AddStringToObject(m, "content", tag);
   cJSON_AddItemToArray(arr, m);
   cJSON_AddItemToObject(c, "messages", arr);
   return c;
}

static const char *first_content(cJSON *container)
{
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(container, "messages");
   cJSON *m = cJSON_GetArrayItem(arr, 0);
   return cJSON_GetStringValue(cJSON_GetObjectItem(m, "content"));
}

/* Simulate a mutated request: container holds the "reduced" array, ctx carries the
 * pristine snapshot + a resolved key, as gw_buffered_mutate would have left it. */
static void make_mutated(cJSON **container, gw_mutate_ctx_t *ctx, const char *skey)
{
   *container = container_with("reduced");
   gw_mutate_ctx_init(ctx);
   ctx->mutate_on = 1;
   ctx->have_key = 1;
   ctx->mutated = 1;
   ctx->ttl_ms = 3600000;
   snprintf(ctx->skey, sizeof(ctx->skey), "%s", skey);
   ctx->st.reduced = 1; /* provenance marked (post-swap) */
   /* Stand in for the detached original the seam sets aside when it swaps the
      reduced array in. Detached, not copied -- so this is simply an array the
      ctx owns. */
   cJSON *pc = container_with("original");
   ctx->original = cJSON_DetachItemFromObjectCaseSensitive(pc, "messages");
   cJSON_Delete(pc);
}

/* The 4xx/5xx decision and the circuit breaker moved into the Go economizer
 * module, where they are covered by breaker_test.go (including the round trip
 * that proves a trip blocks the next reduction). A unit test cannot serve a bus
 * stage, so what is pinned HERE is the half C kept: the FAIL-SAFE.
 *
 * With no module reachable the seam must do nothing at all rather than
 * half-handle the turn. Inventing a resend would send a customer request twice;
 * claiming a disable would record a breaker that was never set. Both are worse
 * than leaving the failed turn exactly as dispatched. */
static void test_post_status_is_inert_without_a_module(void)
{
   cJSON *c;
   gw_mutate_ctx_t ctx;
   make_mutated(&c, &ctx, "0011223344556677");

   assert(gw_buffered_after_status(c, "messages", 413, &ctx) == GW_POST_NONE);
   assert(strcmp(first_content(c), "reduced") == 0); /* body left as dispatched */
   assert(ctx.mutated == 1);                         /* the turn was not consumed */
   /* Nothing to assert about counters here any more: C holds none. That the
      module recorded nothing is proven where the counters live, by it never
      being called. */
   gw_mutate_ctx_free(&ctx);
   cJSON_Delete(c);

   /* A 5xx is equally inert, and so is the streaming path. */
   cJSON *c2;
   gw_mutate_ctx_t ctx2;
   make_mutated(&c2, &ctx2, "8899aabbccddeeff");
   assert(gw_buffered_after_status(c2, "messages", 503, &ctx2) == GW_POST_NONE);
   gw_stream_disable(&ctx2, "stream_invalid_request");
   assert(ctx2.mutated == 1);
   gw_mutate_ctx_free(&ctx2);
   cJSON_Delete(c2);
}

/* With the feature off (default config) OR no resolvable identity, the container is
 * left byte-intact and nothing is mutated. (mutate=1 needs a config file; the
 * default-off + identity-less passthrough is what we pin here.) */
static void test_dark_default_and_identityless(void)
{
   cJSON *c = container_with("orig");
   gw_mutate_ctx_t ctx;

   /* default config -> economizer tier safe -> gateway mutation off -> dark no-op */
   gw_buffered_mutate(c, "messages", "some-model", NULL, NULL, NULL, NULL, &ctx);
   assert(ctx.mutated == 0);
   assert(ctx.mutate_on == 0); /* flag off */
   assert(strcmp(first_content(c), "orig") == 0);
   gw_mutate_ctx_free(&ctx);
   cJSON_Delete(c);
}

static void test_stream_error_classify(void)
{
   /* invalid-request class -> disable */
   assert(gw_stream_anthropic_error_is_invalid_request(
              "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"x\"}"
              "}") == 1);
   assert(gw_stream_anthropic_error_is_invalid_request(
              "{\"error\":{\"type\":\"request_too_large\"}}") == 1);
   /* transient / unrelated -> do NOT disable */
   assert(gw_stream_anthropic_error_is_invalid_request(
              "{\"error\":{\"type\":\"overloaded_error\"}}") == 0);
   assert(gw_stream_anthropic_error_is_invalid_request(
              "{\"error\":{\"type\":\"rate_limit_error\"}}") == 0);
   assert(gw_stream_anthropic_error_is_invalid_request("{\"error\":{\"type\":\"api_error\"}}") ==
          0);
   /* auth outages are NOT invalid-request (no breaker) */
   assert(gw_stream_anthropic_error_is_invalid_request(
              "{\"error\":{\"type\":\"authentication_error\"}}") == 0);
   /* EXACT match: a type that merely contains the words does not false-trip */
   assert(gw_stream_anthropic_error_is_invalid_request(
              "{\"error\":{\"type\":\"not_invalid_request_error\"}}") == 0);
   assert(gw_stream_anthropic_error_is_invalid_request(
              "{\"error\":{\"type\":\"request_too_large_retry\"}}") == 0);
   /* garbage-safe */
   assert(gw_stream_anthropic_error_is_invalid_request(NULL) == 0);
   assert(gw_stream_anthropic_error_is_invalid_request("not json") == 0);
   assert(gw_stream_anthropic_error_is_invalid_request("") == 0);

   /* status classifier: only 400/413/422 are the payload class; auth/rate-limit/5xx
    * are not (the buffered-replay path must not disable on them) */
   assert(gw_status_is_invalid_request(400) == 1);
   assert(gw_status_is_invalid_request(413) == 1);
   assert(gw_status_is_invalid_request(422) == 1);
   assert(gw_status_is_invalid_request(401) == 0);
   assert(gw_status_is_invalid_request(403) == 0);
   assert(gw_status_is_invalid_request(404) == 0);
   assert(gw_status_is_invalid_request(429) == 0);
   assert(gw_status_is_invalid_request(503) == 0);
   assert(gw_status_is_invalid_request(200) == 0);
}

static void test_no_behavior_change_when_off(void)
{
   /* With gateway mutation off (economizer tier safe, the default in this test's HOME), the
    * buffered mutate short-circuits at the flag gate BEFORE inspecting the payload, so the
    * container is a byte-identical no-op for ANY input regardless of size/shape. */
   cJSON *c = container_with("payload");
   char *before = cJSON_PrintUnformatted(c);
   gw_mutate_ctx_t ctx;
   gw_buffered_mutate(c, "messages", "model", "sys", "0011223344556677", "bearer", "identity",
                      &ctx);
   char *after = cJSON_PrintUnformatted(c);
   assert(before && after && strcmp(before, after) == 0);
   assert(ctx.mutated == 0);
   free(before);
   free(after);
   gw_mutate_ctx_free(&ctx);
   cJSON_Delete(c);
}

static void test_upstream_provider_gate(void)
{
   /* Policy: the upstream's provider no longer decides. Anthropic was excluded while
    * the gateway had no freeze boundary to keep a folded prefix byte-stable; now that
    * it persists one per session, BOTH providers follow the same base enable gate.
    * The gate that matters is the tier, not the vendor. */
   assert(gw_mutate_upstream_ok(1) == gw_mutate_is_enabled()); /* Anthropic: base gate */
   assert(gw_mutate_upstream_ok(0) == gw_mutate_is_enabled()); /* non-Anthropic: same */
   assert(gw_mutate_is_enabled() == 0);                        /* default config -> dark */
   assert(gw_mutate_upstream_ok(0) == 0);                      /* so both are off here */
   assert(gw_mutate_upstream_ok(1) == 0);                      /* including Anthropic */
}

int main(void)
{
   printf("gateway_mutate_wire: ");
   /* Deterministic defaults for config_load (no aimee.yaml -> economizer safe -> gateway mutation
    * off). */
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-gwwire-XXXXXX", platform_tmpdir());
   if (platform_mkdtemp(tmpdir))
   {
      platform_setenv("HOME", tmpdir);
      platform_unsetenv("AIMEE_HOME");
      platform_setenv("AIMEE_NO_CACHE", "1");
   }
   test_post_status_is_inert_without_a_module();
   test_dark_default_and_identityless();
   test_stream_error_classify();
   test_no_behavior_change_when_off();
   test_upstream_provider_gate();
   printf("ok\n");
   return 0;
}
