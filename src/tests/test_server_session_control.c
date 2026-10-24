#include "server_session_control.h"
#include "vault_principal.h"
#include <aimee/ir/session_event.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static int results[4];
static void private_execution_alias(void)
{
   server_control_scope_t *scope =
       server_control_scope_open_execution("child", "child-turn", "alice", "parent");
   assert(scope && !server_control_scope_bind(scope));
   server_control_scope_t *lease = NULL;
   assert(!server_control_scope_capture("parent", "alice", &lease) && lease == scope);
   server_control_scope_release(lease);
   assert(!server_control_scope_capture("child", "alice", &lease) && lease == scope);
   server_control_scope_release(lease);
   assert(server_control_scope_capture("parent", "mallory", &lease) < 0 && !lease);
   assert(server_control_scope_capture("other-child", "alice", &lease) < 0 && !lease);
   cJSON *request = cJSON_Parse("{\"tool\":\"write_file\",\"input\":{}}");
   cJSON *reply = cJSON_Parse("{\"decision\":\"deny\"}");
   assert(!server_control_register(scope, "p", "permission", request));
   assert(server_control_reply("parent", "child-turn:p", "alice", reply) == 1);
   assert(server_control_reply("child", "wrong-turn:p", "alice", reply) == 1);
   assert(server_control_reply("child", "child-turn:p", "mallory", reply) < 0);
   assert(!server_control_reply("child", "child-turn:p", "alice", reply));
   server_control_scope_close(scope);
   assert(server_control_reply("child", "child-turn:p", "alice", reply) == 1);
   cJSON_Delete(request);
   cJSON_Delete(reply);
}
static void *answer(void *arg)
{
   size_t n = (size_t)arg;
   cJSON *reply = cJSON_Parse("{\"decision\":\"allow\"}");
   results[n] = server_control_reply("session", "turn:permission", "alice", reply);
   cJSON_Delete(reply);
   return NULL;
}
static void long_owner_reply(void)
{
   char owner[VAULT_PRINCIPAL_MAX] = "webuser:";
   memset(owner + 8, 'a', VAULT_CERT_CN_MAX);
   owner[8 + VAULT_CERT_CN_MAX] = 0;
   server_control_scope_t *scope = server_control_scope_open("long-session", "turn", owner);
   assert(scope);
   cJSON *request = cJSON_Parse("{\"tool_name\":\"Read\",\"input\":{\"path\":\"source.c\"}}");
   cJSON *reply = cJSON_Parse("{\"decision\":\"deny\"}");
   assert(!server_control_register(scope, "permission", "permission", request));
   owner[8 + VAULT_CERT_CN_MAX - 1] = 'b';
   assert(server_control_reply("long-session", "turn:permission", owner, reply) < 0);
   owner[8 + VAULT_CERT_CN_MAX - 1] = 'a';
   assert(!server_control_reply("long-session", "turn:permission", owner, reply));
   char *id = NULL, *kind = NULL;
   cJSON *original = NULL, *decision = NULL;
   assert(server_control_take(scope, "permission", &id, &kind, &original, &decision) == 1);
   assert(!strcmp(id, "permission") && cJSON_Compare(decision, reply, 1));
   free(id);
   free(kind);
   cJSON_Delete(original);
   cJSON_Delete(decision);
   cJSON_Delete(request);
   cJSON_Delete(reply);
   server_control_scope_close(scope);
}

static void *closed_worker(void *arg)
{
   server_control_scope_t *scope = arg;
   assert(!server_control_scope_bind(scope));
   server_control_scope_t *capture = NULL;
   assert(server_control_scope_capture("retained", "alice", &capture) < 0 && !capture);
   cJSON *request = cJSON_Parse("{\"tool_name\":\"Read\",\"input\":{}}");
   assert(server_control_register(scope, "late", "permission", request) < 0);
   char *id = NULL, *kind = NULL;
   cJSON *original = NULL, *reply = NULL;
   assert(server_control_take(scope, NULL, &id, &kind, &original, &reply) < 0);
   assert(!id && !kind && !original && !reply);
   cJSON_Delete(request);
   assert(server_control_scope_bind(NULL) == scope);
   server_control_scope_release(scope);
   return NULL;
}
static void retained_owner_teardown(void)
{
   server_control_scope_t *capture = NULL;
   assert(!server_control_scope_capture(NULL, NULL, &capture) && !capture);
   server_control_scope_t *scope = server_control_scope_open("retained", "turn", "alice");
   assert(scope && !server_control_scope_bind(scope));
   assert(server_control_scope_capture("retained", "mallory", &capture) < 0 && !capture);
   assert(server_control_scope_capture("other", "alice", &capture) < 0 && !capture);
   assert(!server_control_scope_capture("retained", "alice", &capture) && capture == scope);
   /* The lease outlives the creator; the new execution may reuse the identity
    * without a delayed worker registering controls against it. */
   server_control_scope_close(scope);
   server_control_scope_t *replacement = server_control_scope_open("retained", "turn", "alice");
   assert(replacement);
   pthread_t worker;
   assert(!pthread_create(&worker, NULL, closed_worker, capture));
   assert(!pthread_join(worker, NULL));
   server_control_scope_close(replacement);
}

static pthread_mutex_t record_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t record_ready = PTHREAD_COND_INITIALIZER;
static int recording, record_release;
static int blocked_record(const char *session, const char *turn, const char *principal,
                          const aimee_session_event_t *event)
{
   assert(!strcmp(session, "blocked") && !strcmp(turn, "turn") && !strcmp(principal, "alice"));
   assert(!strcmp(event->request_id, "turn:pending"));
   struct timespec until;
   assert(!clock_gettime(CLOCK_REALTIME, &until));
   until.tv_sec += 3;
   pthread_mutex_lock(&record_lock);
   recording = 1;
   pthread_cond_broadcast(&record_ready);
   while (!record_release)
      assert(!pthread_cond_timedwait(&record_ready, &record_lock, &until));
   pthread_mutex_unlock(&record_lock);
   return 0;
}
static void *record_worker(void *arg)
{
   cJSON *request = cJSON_Parse("{\"title\":\"Run?\"}");
   int registered =
       server_control_register_recorded(arg, "pending", "permission", request, blocked_record);
   assert(!registered);
   cJSON_Delete(request);
   return NULL;
}
static void unrelated_scope_remains_responsive(void)
{
   server_control_scope_t *blocked = server_control_scope_open("blocked", "turn", "alice");
   assert(blocked);
   pthread_t worker;
   assert(!pthread_create(&worker, NULL, record_worker, blocked));
   struct timespec until;
   assert(!clock_gettime(CLOCK_REALTIME, &until));
   until.tv_sec += 3;
   pthread_mutex_lock(&record_lock);
   while (!recording)
      assert(!pthread_cond_timedwait(&record_ready, &record_lock, &until));
   pthread_mutex_unlock(&record_lock);
   server_control_scope_t *other = server_control_scope_open("other", "turn", "bob");
   assert(other);
   cJSON *request = cJSON_Parse("{\"title\":\"Independent?\"}");
   cJSON *reply = cJSON_Parse("{\"decision\":\"deny\"}");
   assert(!server_control_register(other, "pending", "permission", request));
   assert(!server_control_reply("other", "turn:pending", "bob", reply));
   char *id = NULL, *kind = NULL;
   cJSON *original = NULL, *decision = NULL;
   assert(server_control_take(other, NULL, &id, &kind, &original, &decision) == 1);
   free(id);
   free(kind);
   cJSON_Delete(original);
   cJSON_Delete(decision);
   cJSON_Delete(request);
   cJSON_Delete(reply);
   server_control_scope_close(other);
   pthread_mutex_lock(&record_lock);
   record_release = 1;
   pthread_cond_broadcast(&record_ready);
   pthread_mutex_unlock(&record_lock);
   assert(!pthread_join(worker, NULL));
   reply = cJSON_Parse("{\"decision\":\"deny\"}");
   assert(!server_control_reply("blocked", "turn:pending", "alice", reply));
   assert(server_control_take(blocked, NULL, &id, &kind, &original, &decision) == 0);
   assert(server_control_take(blocked, "pending", &id, &kind, &original, &decision) == 1);
   free(id);
   free(kind);
   cJSON_Delete(original);
   cJSON_Delete(decision);
   cJSON_Delete(reply);
   server_control_scope_close(blocked);
}

static void host_control_reservation(void)
{
   server_control_scope_t *scope = server_control_scope_open("host-session", "turn", "alice");
   cJSON *request = cJSON_Parse("{\"title\":\"Run?\"}");
   cJSON *changed = cJSON_Parse("{\"title\":\"Changed\"}");
   cJSON *reply = cJSON_Parse("{\"decision\":\"deny\"}");
   assert(scope && request && changed && reply);
   int reserved = server_control_reserve(scope, "host", "permission", request);
   assert(!reserved);
   assert(server_control_reply("host-session", "turn:host", "alice", reply) == 1);
   assert(server_control_register(scope, "host", "permission", changed) < 0);
   int published = server_control_register(scope, "host", "permission", request);
   assert(!published);
   assert(server_control_register(scope, "host", "permission", request) < 0);
   assert(!server_control_reply("host-session", "turn:host", "alice", reply));
   char *id = NULL, *kind = NULL;
   cJSON *original = NULL, *decision = NULL;
   assert(server_control_take(scope, NULL, &id, &kind, &original, &decision) == 0);
   assert(server_control_take(scope, "host", &id, &kind, &original, &decision) == 1);
   assert(!strcmp(id, "host") && cJSON_Compare(original, request, 1));
   free(id);
   free(kind);
   cJSON_Delete(original);
   cJSON_Delete(decision);
   reserved = server_control_reserve(scope, "abandoned", "permission", request);
   assert(!reserved);
   int abandoned = server_control_abandon(scope, "abandoned");
   assert(!abandoned);
   assert(server_control_register(scope, "abandoned", "permission", request) < 0);
   cJSON_Delete(request);
   cJSON_Delete(changed);
   cJSON_Delete(reply);
   server_control_scope_close(scope);
}

static int disabled_record_calls;
static int record_disabled(const char *session, const char *turn, const char *principal,
                           const aimee_session_event_t *event)
{
   (void)session; (void)turn; (void)principal; (void)event;
   disabled_record_calls++;
   return 0;
}
static void headless_control_scope(void)
{
   server_control_scope_t *scope = server_control_scope_open("headless", "turn", "alice");
   assert(scope && server_control_scope_can_reply(scope));
   server_control_scope_disable_replies(scope);
   server_control_scope_disable_replies(scope); /* Restriction is idempotent, never an enable. */
   assert(!server_control_scope_can_reply(scope) && !server_control_scope_failed(scope));
   assert(!server_control_scope_bind(scope));
   server_control_scope_t *lease = NULL;
   assert(!server_control_scope_capture("headless", "alice", &lease) && lease == scope);
   server_control_scope_release(lease); /* Native/capture ownership remains available. */
   cJSON *request = cJSON_Parse("{\"tool\":\"write_file\",\"input\":{}}"),
         *reply = cJSON_Parse("{\"decision\":\"allow\"}");
   assert(server_control_register(scope, "p", "permission", request) < 0);
   assert(server_control_reserve(scope, "q", "question", request) < 0);
   assert(server_control_register_recorded(scope, "r", "permission", request, record_disabled) < 0);
   assert(!disabled_record_calls && server_control_reply("headless", "turn:p", "alice", reply) == 1);
   cJSON_Delete(request);
   cJSON_Delete(reply);
   server_control_scope_close(scope);
   scope = server_control_scope_open("browser", "turn", "alice");
   assert(scope && server_control_scope_can_reply(scope)); /* No cross-scope inheritance. */
   server_control_scope_close(scope);
}

int main(void)
{
   headless_control_scope();
   private_execution_alias();
   host_control_reservation();
   unrelated_scope_remains_responsive();
   retained_owner_teardown();
   long_owner_reply();
   server_control_scope_t *scope = server_control_scope_open("session", "turn", "alice");
   assert(scope);
   assert(server_control_scope_bind(scope) == NULL && server_control_scope_current() == scope);
   cJSON *request = cJSON_Parse("{\"tool_name\":\"Read\",\"input\":{\"path\":\"source.c\"}}");
   assert(server_control_register(scope, "permission", "permission", request) == 0);
   assert(server_control_register(scope, "permission", "permission", request) < 0);
   cJSON *reply = cJSON_Parse("{\"decision\":\"allow\"}");
   assert(server_control_reply("session", "turn:permission", "mallory", reply) < 0);
   assert(server_control_reply("session", "old-turn:permission", "alice", reply) == 1);
   assert(server_control_reply("other-session", "turn:permission", "alice", reply) == 1);
   const char *ambiguous[] = {
       "{\"decision\":\"allow\",\"decision\":\"deny\"}",
       "{\"decision\":\"allow\",\"answers\":{\"a\":[\"x\"],\"a\":[\"y\"]}}",
       "{\"decision\":\"allow\",\"content\":{\"a\":1,\"a\":2}}",
   };
   for (size_t i = 0; i < sizeof ambiguous / sizeof ambiguous[0]; i++)
   {
      cJSON *bad = cJSON_Parse(ambiguous[i]);
      assert(bad && server_control_reply("session", "turn:permission", "alice", bad) < 0);
      cJSON_Delete(bad);
   }
   pthread_t threads[4];
   for (size_t i = 0; i < 4; i++)
      assert(!pthread_create(&threads[i], NULL, answer, (void *)i));
   int accepted = 0;
   for (int i = 0; i < 4; i++)
   {
      pthread_join(threads[i], NULL);
      assert(results[i] == 0 || results[i] == 1);
      accepted += results[i] == 0;
   }
   assert(accepted == 1);
   char *id = NULL, *kind = NULL;
   cJSON *original = NULL, *decision = NULL;
   assert(server_control_take(scope, "wrong-id", &id, &kind, &original, &decision) == 0);
   assert(server_control_take(scope, "permission", &id, &kind, &original, &decision) == 1);
   assert(!strcmp(id, "permission") && !strcmp(kind, "permission") &&
          cJSON_Compare(request, original, 1));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(decision, "decision")), "allow"));
   free(id);
   free(kind);
   cJSON_Delete(original);
   cJSON_Delete(decision);
   assert(server_control_reply("session", "turn:permission", "alice", reply) == 1);
   assert(server_control_take(scope, NULL, &id, &kind, &original, &decision) == 0);
   cJSON_Delete(reply);
   reply = cJSON_Parse("{\"decision\":\"allow\",\"native_response\":{}}");
   assert(server_control_reply("session", "turn:permission", "alice", reply) < 0);
   /* Invalid form submissions do not consume the pending native request. */
   cJSON *form =
       cJSON_Parse("{\"form\":{\"message\":\"Pick options\",\"schema\":{\"type\":\"object\","
                   "\"properties\":{\"count\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4},"
                   "\"name\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":1},"
                   "\"enabled\":{\"type\":\"boolean\"},\"tags\":{\"type\":\"array\",\"minItems\":1,"
                   "\"maxItems\":2,\"items\":{\"anyOf\":[{\"const\":\"a\",\"title\":\"A\"},"
                   "{\"const\":\"b\",\"title\":\"B\"}]}}},\"required\":[\"count\",\"name\","
                   "\"enabled\",\"tags\"]}}}");
   assert(form && server_control_register(scope, "form", "elicitation", form) == 0);
   const char *invalid[] = {
       "{}",
       "{\"count\":1.5,\"name\":\"x\",\"enabled\":true,\"tags\":[\"a\"]}",
       "{\"count\":5,\"name\":\"x\",\"enabled\":true,\"tags\":[\"a\"]}",
       "{\"count\":2,\"name\":\"xx\",\"enabled\":true,\"tags\":[\"a\"]}",
       "{\"count\":2,\"name\":\"x\",\"enabled\":1,\"tags\":[\"a\"]}",
       "{\"count\":2,\"name\":\"x\",\"enabled\":true,\"tags\":[\"unknown\"]}",
       "{\"count\":2,\"name\":\"x\",\"enabled\":true,\"tags\":[\"a\",\"a\"]}",
       "{\"count\":2,\"name\":\"x\",\"enabled\":true,\"tags\":[\"a\"],\"extra\":true}",
   };
   for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++)
   {
      cJSON *bad = cJSON_CreateObject();
      cJSON_AddStringToObject(bad, "decision", "allow");
      cJSON_AddItemToObject(bad, "content", cJSON_Parse(invalid[i]));
      assert(server_control_reply("session", "turn:form", "alice", bad) < 0);
      assert(server_control_take(scope, "form", &id, &kind, &original, &decision) == 0);
      cJSON_Delete(bad);
   }
   cJSON *accepted_form = cJSON_Parse("{\"decision\":\"allow\",\"content\":{\"count\":2,"
                                      "\"name\":\"é\",\"enabled\":false,\"tags\":[\"a\",\"b\"]}}");
   assert(server_control_reply("session", "turn:form", "mallory", accepted_form) < 0);
   assert(server_control_reply("session", "turn:form", "alice", accepted_form) == 0);
   assert(server_control_take(scope, "form", &id, &kind, &original, &decision) == 1);
   assert(cJSON_Compare(accepted_form, decision, 1));
   free(id);
   free(kind);
   cJSON_Delete(original);
   cJSON_Delete(decision);
   cJSON_Delete(form);
   cJSON_Delete(accepted_form);
   cJSON *url = cJSON_Parse("{\"url\":{\"href\":\"https://accounts.example.test/"
                            "authorize\",\"message\":\"Authorize\",\"server\":\"inventory\","
                            "\"elicitation_id\":\"el-1\"}}");
   assert(url && server_control_register(scope, "url", "elicitation", url) == 0);
   cJSON *url_reply = cJSON_Parse("{\"decision\":\"allow\",\"content\":{}}");
   assert(server_control_reply("session", "turn:url", "alice", url_reply) < 0);
   cJSON_DeleteItemFromObjectCaseSensitive(url_reply, "content");
   assert(server_control_reply("session", "turn:url", "mallory", url_reply) < 0);
   assert(server_control_reply("session", "turn:url", "alice", url_reply) == 0);
   assert(server_control_take(scope, "url", &id, &kind, &original, &decision) == 1);
   assert(cJSON_Compare(original, url, 1) && cJSON_Compare(decision, url_reply, 1));
   free(id);
   free(kind);
   cJSON_Delete(original);
   cJSON_Delete(decision);
   cJSON *href = cJSON_GetObjectItem(cJSON_GetObjectItem(url, "url"), "href");
   cJSON_SetValuestring(href, "HTTPS://accounts.example.test/authorize");
   assert(aimee_ir_elicitation_url_valid(cJSON_GetObjectItem(url, "url")));
   const char *bad_urls[] = {"javascript:alert(1)",
                             "data:text/html,secret",
                             "file:///etc/passwd",
                             "https://user:password@example.test/",
                             "https://",
                             "https://x\\@attacker/",
                             "https://example.test/\nheader"};
   for (size_t i = 0; i < sizeof bad_urls / sizeof bad_urls[0]; i++)
   {
      cJSON_SetValuestring(href, bad_urls[i]);
      assert(!aimee_ir_elicitation_url_valid(cJSON_GetObjectItem(url, "url")));
   }
   cJSON_Delete(url);
   cJSON_Delete(url_reply);
   server_control_scope_close(scope);
   assert(!server_control_scope_current());
   cJSON_Delete(reply);
   reply = cJSON_Parse("{\"decision\":\"deny\"}");
   assert(server_control_reply("session", "turn:permission", "alice", reply) == 1);
   cJSON_Delete(reply);
   cJSON_Delete(request);
   puts("server controls: owner/turn/session binding, concurrent reply, consume-once and teardown "
        "passed");
   return 0;
}
