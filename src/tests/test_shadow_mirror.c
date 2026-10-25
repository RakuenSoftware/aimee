/* test_shadow_mirror.c -- shadow-traffic publishing to dynamic subscribers.
 *
 * The mirror must (a) only fire for completion paths, (b) never fire for a request
 * that is itself a mirror (the loop guard -- get this wrong and two peers pointed at
 * each other melt down), (c) deliver the exact body with the X-Aimee-Shadow marker
 * to EVERY subscriber's ingress, (d) do nothing when publishing is off or there are
 * no subscribers, and (e) self-prune a subscriber whose deliveries keep failing.
 * agent_http_post is stubbed so the delivery path is exercised without real peers;
 * the stub captures what was sent and can be made to fail on demand. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aimee.h"
#include "shadow_mirror.h"
#include <aimee/translation/aimee_frontend.h>
#include "cJSON.h"
#include "log.h"
void aimee_log(log_level_t level, const char *module, const char *format, ...)
{
   (void)level;
   (void)module;
   (void)format;
}

/* ---- stub agent_http_post: capture deliveries, optionally fail ------------- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_calls;
static int g_fail; /* when set, every delivery returns HTTP503 */
static char g_last_url[600];
static char g_last_auth[300];
static char g_last_body[4096];
static char g_last_extra[200];

int agent_http_post(const char *url, const char *auth_header, const char *body, char **response_buf,
                    int timeout_ms, const char *extra_headers)
{
   (void)timeout_ms;
   pthread_mutex_lock(&g_lock);
   g_calls++;
   snprintf(g_last_url, sizeof(g_last_url), "%s", url ? url : "");
   snprintf(g_last_auth, sizeof(g_last_auth), "%s", auth_header ? auth_header : "");
   snprintf(g_last_body, sizeof(g_last_body), "%s", body ? body : "");
   snprintf(g_last_extra, sizeof(g_last_extra), "%s", extra_headers ? extra_headers : "");
   int fail = g_fail;
   pthread_mutex_unlock(&g_lock);
   if (response_buf)
      *response_buf = NULL;
   return fail ? 503 : 200;
}

static int calls(void)
{
   pthread_mutex_lock(&g_lock);
   int c = g_calls;
   pthread_mutex_unlock(&g_lock);
   return c;
}
static void set_fail(int f)
{
   pthread_mutex_lock(&g_lock);
   g_fail = f;
   pthread_mutex_unlock(&g_lock);
}
static void reset_calls(void)
{
   pthread_mutex_lock(&g_lock);
   g_calls = 0;
   pthread_mutex_unlock(&g_lock);
}

/* Deliveries run on detached threads; wait (bounded) for `want` of them. */
static int wait_for_calls(int want)
{
   for (int i = 0; i < 300; i++)
   {
      if (calls() >= want)
         return 1;
      struct timespec ts = {0, 10 * 1000 * 1000}; /* 10ms */
      nanosleep(&ts, NULL);
   }
   return calls() >= want;
}

static void settle(void)
{
   struct timespec ts = {0, 60 * 1000 * 1000};
   nanosleep(&ts, NULL);
}

static const char valid[] =
    "{\"model\":\"x\",\"max_tokens\":16,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
#ifdef SHADOW_MIRROR_BASELINE
extern void shadow_mirror_dispatch(const char *, const char *, int, int);
#endif
static void dispatch(const char *path, const char *body, int loop)
{
#ifdef SHADOW_MIRROR_BASELINE
   shadow_mirror_dispatch(path, body, (int)strlen(body), loop);
#else
   shadow_mirror_begin_request(path, loop);
   aimee_request_t request = {0};
   aimee_wire_t wire = !strcmp(path, "/v1/messages")    ? AIMEE_WIRE_ANTHROPIC
                       : !strcmp(path, "/v1/responses") ? AIMEE_WIRE_RESPONSES
                                                        : AIMEE_WIRE_OPENAI_CHAT;
   int rc = !strcmp(path, "/v1/completions")
                ? aimee_frontend_parse_text_completion(body, strlen(body), &request, NULL, 0)
                : aimee_frontend_parse_wire(body, strlen(body), wire, &request, NULL, 0);
   if (!rc && !aimee_ir_request_admit(&request))
   {
      shadow_mirror_capture_request(&request);
      shadow_mirror_dispatch_admitted(&request);
      shadow_mirror_dispatch_admitted(&request); /* same ingress is at most once */
   }
   aimee_request_free(&request);
   shadow_mirror_end_request();
#endif
}

#ifndef SHADOW_MIRROR_BASELINE
static void test_owned_scope(void)
{
   shadow_mirror_reset();
   shadow_mirror_set_armed(1);
   assert(!shadow_mirror_subscribe("https://a.local:8743", "tok-a"));
   reset_calls();
   aimee_request_t outer = {0}, nested = {0};
   assert(!aimee_frontend_parse_wire(valid, strlen(valid), AIMEE_WIRE_ANTHROPIC, &outer, NULL, 0));
   assert(!aimee_ir_request_clone(&outer, &nested));
   shadow_mirror_capture_request(&outer); /* no HTTP scope */
   shadow_mirror_dispatch_admitted(&outer);
   settle();
   assert(!calls());
   shadow_mirror_begin_request("/v1/messages", 0);
   shadow_mirror_capture_request(&outer);
   shadow_mirror_capture_request(&nested);
   shadow_mirror_dispatch_admitted(&nested);
   settle();
   assert(!calls());
   /* Local policy/model mutation cannot change the already admitted inbound
    * snapshot or be applied a second time by the subscriber. */
   free(outer.model);
   outer.model = strdup("local-post-policy");
   shadow_mirror_dispatch_admitted(&outer);
   shadow_mirror_dispatch_admitted(&outer);
   assert(wait_for_calls(1));
   settle();
   assert(calls() == 1 && !strcmp(g_last_body, valid));
   shadow_mirror_end_request();
   reset_calls();
   shadow_mirror_begin_request("/v1/messages", 0);
   shadow_mirror_capture_request(&nested);
   shadow_mirror_end_request(); /* rejected local preparation / early exit */
   shadow_mirror_dispatch_admitted(&nested);
   settle();
   assert(!calls());
   aimee_request_free(&nested);
   aimee_request_free(&outer);
   puts("  PASS: admitted inbound snapshot fidelity, nested isolation, one-shot and teardown");
}
static void test_unsupported_and_opaque(void)
{
   const char *bad[] = {
       "{\"model\":\"x\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"n\":2}",
       "{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"temperature\":\"ignored\"}",
       "{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"tool_choice\":{\"type\":"
       "\"function\",\"function\":{\"name\":\"f\",\"ignored\":true}},\"tools\":[{\"type\":"
       "\"function\",\"function\":{\"name\":\"f\",\"parameters\":{\"type\":\"object\"}}}]}",
       "{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"chat_template_kwargs\":{\"ignored_"
       "control\":true}}"};
   for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
   {
      reset_calls();
      dispatch("/v1/chat/completions", bad[i], 0);
      settle();
      assert(!calls());
   }
   reset_calls();
   dispatch("/v1/completions", "{\"prompt\":\"hello\",\"n\":2}", 0);
   dispatch("/v1/completions", "{\"prompt\":\"hello\",\"unknown_control\":true}", 0);
   settle();
   assert(!calls());
   const char *opaque = "{\"model\":\"x\",\"stream\":true,\"store\":false,\"input\":[{\"type\":"
                        "\"reasoning\",\"id\":\"r1\",\"encrypted_content\":\"opaque-state\","
                        "\"summary\":[]},{\"role\":\"user\",\"content\":\"hi\"}]}";
   dispatch("/v1/responses", opaque, 0);
   assert(wait_for_calls(1));
   settle();
   assert(calls() == 1 && strstr(g_last_body, "opaque-state"));
   reset_calls();
   dispatch("/v1/responses", "{\"input\":\"hi\",\"previous_response_id\":\"foreign-local-id\"}", 0);
   settle();
   assert(!calls());
   /* HTTP200 is delivery, regardless of a discarded provider diagnostic body.
    * Repeated successful deliveries must never self-prune the subscriber. */
   dispatch("/v1/completions", "{\"prompt\":\"hi\",\"stop\":\"END\",\"temperature\":null}", 0);
   assert(wait_for_calls(1));
   settle();
   assert(calls() == 1);
   cJSON *legacy = cJSON_Parse(g_last_body);
   assert(cJSON_GetArraySize(cJSON_GetObjectItem(legacy, "stop")) == 1);
   cJSON_Delete(legacy);
   reset_calls();
   for (int i = 0; i < 12; i++)
   {
      dispatch("/v1/messages", valid, 0);
      settle();
   }
   assert(shadow_mirror_subscriber_count() == 1 && calls() == 12);
   reset_calls();
   const char *private_body = "{\"prompt\":\"private source text\"}";
   aimee_request_t private_request = {0};
   assert(!aimee_frontend_parse_text_completion(private_body, strlen(private_body),
                                               &private_request, NULL, 0));
   aimee_ir_block_set_context(&private_request.messages[0].blocks[0], AIMEE_CTX_ORIGIN_USER,
                              AIMEE_CTX_AUTH_EVIDENCE, AIMEE_CTX_TRUST_UNTRUSTED,
                              AIMEE_CTX_SENS_RESTRICTED, 0, "private-source", "test", 1);
   shadow_mirror_begin_request("/v1/completions", 0);
   shadow_mirror_capture_request(&private_request);
   shadow_mirror_dispatch_admitted(&private_request);
   settle();
   shadow_mirror_end_request();
   assert(!calls() && !strcmp(private_request.messages[0].blocks[0].text, "private source text"));
   aimee_request_free(&private_request);
   puts("  PASS: unsupported options refused, opaque state retained, legacy normalization and "
        "HTTP200 delivery");
}
static long allocation_index, fail_at;
static void *fail_one_json_allocation(size_t size)
{
   if (allocation_index++ == fail_at)
      return NULL;
   return malloc(size);
}
static void test_legacy_allocation_atomicity(void)
{
   const char *body = "{\"model\":\"x\",\"prompt\":\"legacy "
                      "text\",\"max_tokens\":17,\"stop\":[\"END\"],\"metadata\":{\"tag\":\"owned\"}"
                      ",\"n\":1,\"best_of\":1,\"echo\":false}";
   for (int i = 0; i < 150; i++)
   {
      reset_calls();
      aimee_request_t request = {0};
      assert(!aimee_frontend_parse_text_completion(body, strlen(body), &request, NULL, 0));
      assert(request.raw && cJSON_GetObjectItemCaseSensitive(request.raw, "max_tokens"));
      shadow_mirror_begin_request("/v1/completions", 0);
      shadow_mirror_capture_request(&request);
      allocation_index = 0;
      fail_at = i;
      cJSON_Hooks hooks = {.malloc_fn = fail_one_json_allocation, .free_fn = free};
      cJSON_InitHooks(&hooks);
      shadow_mirror_dispatch_admitted(&request);
      cJSON_InitHooks(NULL);
      shadow_mirror_end_request();
      aimee_request_free(&request);
      settle();
      assert(calls() <= 1);
      if (calls())
      {
         cJSON *sent = cJSON_Parse(g_last_body);
         assert(sent && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(sent, "model")), "x"));
         assert(cJSON_GetObjectItem(sent, "max_tokens")->valueint == 17);
         assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(sent, "prompt")), "legacy text"));
         assert(cJSON_GetArraySize(cJSON_GetObjectItem(sent, "stop")) == 1);
         assert(cJSON_IsObject(cJSON_GetObjectItem(sent, "metadata")));
         cJSON_Delete(sent);
      }
   }
   puts("  PASS: 150 allocation failures retain complete bounded legacy request or send nothing");
}
#endif

int main(void)
{
   printf("shadow-mirror:\n");

   /* The safety property: publishing is DISARMED at process start, with no config
    * or env able to change that. Enabling is a runtime-only action. */
   assert(!shadow_mirror_publish_enabled());
   printf("  PASS: boots disarmed (no persistent enable)\n");

   /* Path classification. */
   assert(shadow_mirror_is_mirrorable_path("/v1/messages"));
   assert(shadow_mirror_is_mirrorable_path("/v1/chat/completions"));
   assert(!shadow_mirror_is_mirrorable_path("/v1/agents"));
   assert(!shadow_mirror_is_mirrorable_path(NULL));
   printf("  PASS: only completion paths are mirrorable\n");

   /* Disarmed: even with a subscriber registered, no delivery happens. */
   shadow_mirror_reset();
   assert(!shadow_mirror_publish_enabled());
   shadow_mirror_subscribe("https://a.local:8743", "tok-a");
   dispatch("/v1/messages", valid, 0);
   settle();
   assert(calls() == 0 && shadow_mirror_sent_count() == 0);
   printf("  PASS: disarmed -> nothing delivered even with a subscriber\n");

   /* Arm publishing at runtime. */
   shadow_mirror_reset();
   shadow_mirror_set_armed(1);
   assert(shadow_mirror_publish_enabled());

   /* No subscribers -> nothing delivered. */
   dispatch("/v1/messages", valid, 0);
   settle();
   assert(calls() == 0);
   printf("  PASS: no subscribers -> nothing delivered\n");

   assert(shadow_mirror_subscribe("https://a.local:8743", "tok-a") == 0);
   const char *bad[] = {"{}", "{\"model\":\"x\",\"model\":\"y\",\"messages\":[]}",
                        "{\"messages\":[{\"role\":\"user\",\"content\":\"x\"}],\"tool_choice\":{"
                        "\"type\":\"not-supported\"}}"};
   for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
   {
      dispatch("/v1/chat/completions", bad[i], 0);
      settle();
      assert(calls() == 0);
   }
   assert(shadow_mirror_unsubscribe("https://a.local:8743") == 0);

   /* Register two subscribers; a completion request fans out to BOTH, each with the
    * exact body, its own bearer, the shadow marker, and the original path. */
   reset_calls();
   assert(shadow_mirror_subscribe("https://a.local:8743", "tok-a") == 0);
   assert(shadow_mirror_subscribe("https://b.local:8743", "tok-b") == 0);
   assert(shadow_mirror_subscriber_count() == 2);
   dispatch("/v1/messages", valid, 0);
   assert(wait_for_calls(2));
   pthread_mutex_lock(&g_lock);
   /* last-write wins in the stub, but both must have hit /v1/messages with the body
    * and the marker; the URL is one of the two subscribers. */
   assert(strstr(g_last_url, "/v1/messages") != NULL);
   assert(strcmp(g_last_body, valid) == 0);
   assert(strstr(g_last_extra, "X-Aimee-Shadow: 1") != NULL);
   assert(strstr(g_last_auth, "Bearer tok-") != NULL);
   pthread_mutex_unlock(&g_lock);
   assert(shadow_mirror_sent_count() >= 2);
   printf("  PASS: request fans out to every subscriber's real ingress\n");

   /* Loop guard: a request already marked shadow is NEVER re-mirrored. */
   reset_calls();
   dispatch("/v1/messages", valid, 1);
   settle();
   assert(calls() == 0);
   printf("  PASS: a mirrored (X-Aimee-Shadow) request is not re-mirrored\n");

   /* Non-completion path is not delivered. */
   reset_calls();
   dispatch("/v1/agents", "{}", 0);
   settle();
   assert(calls() == 0);
   printf("  PASS: non-completion path is not delivered\n");

   /* Unsubscribe removes a subscriber from the fan-out. */
   assert(shadow_mirror_unsubscribe("https://b.local:8743") == 0);
   assert(shadow_mirror_subscriber_count() == 1);
   printf("  PASS: unsubscribe removes a subscriber\n");

   /* Disarming is a hard stop: it forgets every subscriber. */
   shadow_mirror_set_armed(0);
   assert(!shadow_mirror_publish_enabled());
   assert(shadow_mirror_subscriber_count() == 0);
   printf("  PASS: disarm clears all subscribers\n");

   /* Self-prune: a subscriber whose deliveries keep failing is dropped. Re-arm,
    * register a dead peer, make every delivery fail, and drive dispatches until it
    * is pruned. */
   shadow_mirror_reset();
   shadow_mirror_set_armed(1);
   assert(shadow_mirror_subscribe("https://dead.local:8743", "tok-d") == 0);
   assert(shadow_mirror_subscriber_count() == 1);
   set_fail(1);
   for (int i = 0; i < 40 && shadow_mirror_subscriber_count() > 0; i++)
   {
      dispatch("/v1/messages", valid, 0);
      settle();
   }
   assert(shadow_mirror_subscriber_count() == 0); /* pruned */
   assert(shadow_mirror_pruned_count() >= 1);
   set_fail(0);
   printf("  PASS: a subscriber that keeps failing delivery is self-pruned\n");

#ifndef SHADOW_MIRROR_BASELINE
   test_owned_scope();
   test_unsupported_and_opaque();
   test_legacy_allocation_atomicity();
#endif
   printf("shadow-mirror: ok\n");
   return 0;
}
