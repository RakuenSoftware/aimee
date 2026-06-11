/* test_request_context.c: unit tests for the #3 per-request context store. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "request_context.h"

#define PASS(name) printf("  %s: ok\n", name)

static void test_unset_defaults(void)
{
   request_context_clear();
   assert(request_context_get() == NULL);
   /* Convenience accessors never return NULL even with no context. */
   assert(request_context_idempotency_key() != NULL);
   assert(request_context_idempotency_key()[0] == '\0');
   assert(request_context_principal() != NULL);
   assert(request_context_principal()[0] == '\0');
   PASS("context: unset -> empty, never NULL");
}

static void test_set_get_roundtrip(void)
{
   request_context_t ctx;
   memset(&ctx, 0, sizeof(ctx));
   snprintf(ctx.method, sizeof(ctx.method), "%s", "POST");
   snprintf(ctx.path, sizeof(ctx.path), "%s", "/v1/chat/completions");
   snprintf(ctx.request_id, sizeof(ctx.request_id), "%s", "1234-7");
   snprintf(ctx.idempotency_key, sizeof(ctx.idempotency_key), "%s", "idem-abc");
   snprintf(ctx.session_key, sizeof(ctx.session_key), "%s", "sess-77");
   snprintf(ctx.principal, sizeof(ctx.principal), "%s", "uid:1000");
   snprintf(ctx.source, sizeof(ctx.source), "%s", "openai-ingress");
   ctx.peer_uid = 1000;
   ctx.transport = REQ_TRANSPORT_UDS;
   ctx.capabilities = 0x7u;
   ctx.trusted = 1;
   request_context_set(&ctx);

   const request_context_t *got = request_context_get();
   assert(got != NULL);
   assert(strcmp(got->method, "POST") == 0);
   assert(strcmp(got->path, "/v1/chat/completions") == 0);
   assert(strcmp(got->request_id, "1234-7") == 0);
   assert(strcmp(got->idempotency_key, "idem-abc") == 0);
   assert(strcmp(got->session_key, "sess-77") == 0);
   assert(strcmp(got->principal, "uid:1000") == 0);
   assert(got->peer_uid == 1000);
   assert(got->transport == REQ_TRANSPORT_UDS);
   assert(got->capabilities == 0x7u);
   assert(got->trusted == 1);
   assert(strcmp(request_context_idempotency_key(), "idem-abc") == 0);
   assert(strcmp(request_context_principal(), "uid:1000") == 0);
   PASS("context: set/get roundtrip + accessors");
}

static void test_clear_and_null_set(void)
{
   request_context_t ctx;
   memset(&ctx, 0, sizeof(ctx));
   snprintf(ctx.idempotency_key, sizeof(ctx.idempotency_key), "%s", "x");
   request_context_set(&ctx);
   assert(request_context_get() != NULL);

   /* set(NULL) clears. */
   request_context_set(NULL);
   assert(request_context_get() == NULL);
   assert(request_context_idempotency_key()[0] == '\0');

   request_context_set(&ctx);
   request_context_clear();
   assert(request_context_get() == NULL);
   PASS("context: clear + set(NULL)");
}

int main(void)
{
   printf("request_context: unit tests\n");
   test_unset_defaults();
   test_set_get_roundtrip();
   test_clear_and_null_set();
   printf("All request_context tests passed.\n");
   return 0;
}
