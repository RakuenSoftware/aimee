/* test_request_context.c: unit tests for the #3 per-request context store. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
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
   assert(request_context_caller_subject()[0] == '\0');
   assert(request_context_caller_authorization()[0] == '\0');
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
   snprintf(ctx.caller_subject, sizeof(ctx.caller_subject), "%s", "aimee");
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
   assert(strcmp(request_context_caller_subject(), "aimee") == 0);
   request_context_override_caller_subject("oidc:https%3A//idp:user-7");
   assert(strcmp(request_context_caller_subject(), "oidc:https%3A//idp:user-7") == 0);
   request_context_override_caller_authorization("header.payload.signature");
   assert(strcmp(request_context_caller_authorization(), "header.payload.signature") == 0);
   assert(request_context_caller_subject()[0] == '\0');
   request_context_override_caller_subject("alice");
   assert(strcmp(request_context_caller_subject(), "alice") == 0);
   assert(request_context_caller_authorization()[0] == '\0');
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

static void test_budget_header(void)
{
   request_context_t ctx = {0};
   request_context_capture_budget_header(
       &ctx, "POST /v1/responses HTTP/1.1\r\nx-aimee-context-limits: "
             "\t{\"schema_version\":1,\"max_request_bytes\":0}\r\n\r\n{}");
   assert(ctx.request_budget_present == 1);
   assert(strcmp(ctx.request_budget_limits, "{\"schema_version\":1,\"max_request_bytes\":0}") == 0);
   request_context_set(&ctx);
   memset(&ctx, 0, sizeof(ctx));
   assert(request_context_get()->request_budget_present == 1);
   assert(strstr(request_context_get()->request_budget_limits, "max_request_bytes") != NULL);
   request_context_clear();
   assert(request_context_get() == NULL);
   request_context_capture_budget_header(
       &ctx, "POST / HTTP/1.1\r\nX-Aimee-Context-Limits: {}\r\nX-Aimee-Context-Limits: {}\r\n\r\n");
   assert(ctx.request_budget_present == -1 && ctx.request_budget_limits[0] == 0);
   request_context_capture_budget_header(&ctx,
                                         "POST / HTTP/1.1\r\nX-Aimee-Context-Limits: \t\r\n\r\n");
   assert(ctx.request_budget_present == -1);
   request_context_capture_budget_header(&ctx,
                                         "POST / HTTP/1.1\r\n\r\nX-Aimee-Context-Limits: {}\r\n");
   assert(ctx.request_budget_present == 0);
   char request[1200];
   memset(request, 'x', sizeof(request));
   const char *prefix = "POST / HTTP/1.1\r\nX-Aimee-Context-Limits: ";
   memcpy(request, prefix, strlen(prefix));
   memcpy(request + strlen(prefix) + 1024, "\r\n\r\n", 5);
   request_context_capture_budget_header(&ctx, request);
   assert(ctx.request_budget_present == 1 && strlen(ctx.request_budget_limits) == 1024);
   request[strlen(prefix) + 1024] = 'x';
   memcpy(request + strlen(prefix) + 1025, "\r\n\r\n", 5);
   request_context_capture_budget_header(&ctx, request);
   assert(ctx.request_budget_present == -1 && ctx.request_budget_limits[0] == 0);
   request_context_capture_budget_header(
       &ctx, "POST / HTTP/1.1\r\nX-Aimee-Context-Limits: {}\r\n extra\r\n\r\n");
   assert(ctx.request_budget_present == -1 && ctx.request_budget_limits[0] == 0);
   request_context_capture_budget_header(&ctx, NULL);
   assert(ctx.request_budget_present == 0);
   PASS("context: budget header is bounded, copied, cleared and duplicate-safe");
}

static void *refusal_worker(void *copied)
{
   assert(request_context_get() == NULL);
   request_context_set(copied);
   assert(request_context_get()->context_refused);
   assert(strcmp(request_context_get()->context_refusal_kind, "protected_context_overflow") == 0);
   request_context_clear();
   request_context_t fresh = {0};
   request_context_set(&fresh);
   assert(!request_context_get()->context_refused);
   assert(request_context_refuse_assembly("permission_denied") == 0);
   request_context_clear();
   return NULL;
}
static void test_assembly_refusal_lifetime(void)
{
   request_context_clear();
   assert(request_context_refuse_assembly("unavailable") == -1);
   assert(request_context_get() == NULL);
   request_context_t fresh = {0};
   request_context_set(&fresh);
   assert(request_context_refuse_assembly("protected_context_overflow") == 0);
   assert(request_context_refuse_assembly("unavailable") == 0);
   assert(strcmp(request_context_get()->context_refusal_kind, "protected_context_overflow") == 0);
   request_context_t copy = *request_context_get();
   pthread_t worker;
   assert(pthread_create(&worker, NULL, refusal_worker, &copy) == 0);
   assert(pthread_join(worker, NULL) == 0);
   assert(strcmp(request_context_get()->context_refusal_kind, "protected_context_overflow") == 0);
   request_context_clear();
   request_context_set(&fresh);
   assert(!request_context_get()->context_refused &&
          !request_context_get()->context_refusal_kind[0]);
   char oversized[128];
   memset(oversized, 'x', sizeof(oversized) - 1);
   oversized[sizeof(oversized) - 1] = 0;
   assert(request_context_refuse_assembly(oversized) == 0);
   assert(strcmp(request_context_get()->context_refusal_kind, "unavailable") == 0);
   request_context_clear();
   PASS("context: first refusal survives worker copy without leaking into another request");
}
int main(void)
{
   test_assembly_refusal_lifetime();
   test_budget_header();
   printf("request_context: unit tests\n");
   test_unset_defaults();
   test_set_get_roundtrip();
   test_clear_and_null_set();
   printf("All request_context tests passed.\n");
   return 0;
}
