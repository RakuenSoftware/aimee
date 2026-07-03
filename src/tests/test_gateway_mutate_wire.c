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
#include "gw_mutate_stats.h"
#include "msg_session_disable.h"
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
   ctx->st.reduced = 1; /* provenance marked (post-replace) */
   cJSON *pc = container_with("pristine");
   ctx->pristine = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(pc, "messages"), 1);
   cJSON_Delete(pc);
}

static void test_4xx_restore_resend(void)
{
   msg_session_reset();
   gw_stat_reset();
   cJSON *c;
   gw_mutate_ctx_t ctx;
   const char *skey = "0011223344556677";
   make_mutated(&c, &ctx, skey);

   assert(strcmp(first_content(c), "reduced") == 0);
   gw_post_action_t act = gw_buffered_after_status(c, "messages", 413, &ctx);
   assert(act == GW_POST_RESEND);
   assert(strcmp(first_content(c), "pristine") == 0); /* restored */
   assert(msg_session_is_disabled(skey) == 1);        /* disabled */
   assert(ctx.st.reduced == 0);                       /* provenance cleared */
   assert(ctx.mutated == 0);                          /* no double handling */
   assert(gw_stat_get(GW_STAT_4XX_RESTORE_RESEND) == 1);
   assert(gw_stat_get_reason("session_disabled_set", "4xx") == 1);

   gw_mutate_ctx_free(&ctx);
   cJSON_Delete(c);
}

static void test_5xx_disable_no_resend(void)
{
   msg_session_reset();
   gw_stat_reset();
   cJSON *c;
   gw_mutate_ctx_t ctx;
   const char *skey = "8899aabbccddeeff";
   make_mutated(&c, &ctx, skey);

   gw_post_action_t act = gw_buffered_after_status(c, "messages", 503, &ctx);
   assert(act == GW_POST_NONE);
   assert(strcmp(first_content(c), "reduced") == 0); /* NOT restored — no resend */
   assert(msg_session_is_disabled(skey) == 1);       /* disabled */
   assert(ctx.st.reduced == 0);
   assert(gw_stat_get(GW_STAT_5XX_DISABLE) == 1);
   assert(gw_stat_get_reason("session_disabled_set", "5xx") == 1);

   gw_mutate_ctx_free(&ctx);
   cJSON_Delete(c);
}

static void test_2xx_and_nonmutated_noop(void)
{
   msg_session_reset();
   gw_stat_reset();
   cJSON *c;
   gw_mutate_ctx_t ctx;
   const char *skey = "1234567890abcdef";
   make_mutated(&c, &ctx, skey);

   /* 200: no state change, no disable, stays reduced */
   assert(gw_buffered_after_status(c, "messages", 200, &ctx) == GW_POST_NONE);
   assert(strcmp(first_content(c), "reduced") == 0);
   assert(msg_session_is_disabled(skey) == 0);
   gw_mutate_ctx_free(&ctx);
   cJSON_Delete(c);

   /* a non-mutated request is entirely inert */
   cJSON *c2 = container_with("reduced");
   gw_mutate_ctx_t ctx2;
   gw_mutate_ctx_init(&ctx2);
   assert(gw_buffered_after_status(c2, "messages", 400, &ctx2) == GW_POST_NONE);
   assert(msg_session_count() == 0);
   gw_mutate_ctx_free(&ctx2);
   cJSON_Delete(c2);
}

/* With the feature off (default config) OR no resolvable identity, the container is
 * left byte-intact and nothing is mutated. (mutate=1 needs a config file; the
 * default-off + identity-less passthrough is what we pin here.) */
static void test_dark_default_and_identityless(void)
{
   gw_stat_reset();
   cJSON *c = container_with("orig");
   gw_mutate_ctx_t ctx;

   /* default config -> reduce_gateway_mutate off -> dark no-op */
   gw_buffered_mutate(c, "messages", "some-model", NULL, NULL, NULL, NULL, &ctx);
   assert(ctx.mutated == 0);
   assert(ctx.mutate_on == 0); /* flag off */
   assert(strcmp(first_content(c), "orig") == 0);
   gw_mutate_ctx_free(&ctx);
   cJSON_Delete(c);
}

int main(void)
{
   printf("gateway_mutate_wire: ");
   /* Deterministic defaults for config_load (no aimee.yaml -> reduce_gateway_mutate off). */
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-gwwire-XXXXXX", platform_tmpdir());
   if (platform_mkdtemp(tmpdir))
   {
      platform_setenv("HOME", tmpdir);
      platform_unsetenv("AIMEE_HOME");
      platform_setenv("AIMEE_NO_CACHE", "1");
   }
   test_4xx_restore_resend();
   test_5xx_disable_no_resend();
   test_2xx_and_nonmutated_noop();
   test_dark_default_and_identityless();
   printf("ok\n");
   return 0;
}
