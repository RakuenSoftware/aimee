/* test_turn_registry.c: unit tests for the per-turn cancel registry
 * (server-owned turn lifecycle, Phase 1). */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "turn_registry.h"
#include "server.h"
#include <aimee/ir/session_event.h>

/* --- stub for presence_session_owner (turn_registry_cancel's authz check) --- */
static char g_stub_owner[64];
int presence_session_owner(const char *session_id, char *out, size_t out_n)
{
   (void)session_id;
   if (out && out_n)
      out[0] = '\0';
   if (!g_stub_owner[0])
      return 0;
   if (out && out_n)
      snprintf(out, out_n, "%s", g_stub_owner);
   return 1;
}

static void test_publish_and_collision(void)
{
   turn_entry_t *a = turn_registry_publish("sess-A", "turn-1");
   assert(a != NULL);
   assert(strcmp(a->session_id, "sess-A") == 0);
   assert(turn_registry_match_exact("sess-A", "turn-1") == 1);
   assert(turn_registry_match_exact("sess-A", "stale") == -1);
   assert(turn_registry_match_exact("absent", "turn-1") == 0);
   assert(!turn_entry_cancelled(a));
   assert(atomic_load(&a->cancel) == 0);
   /* A second publish for the same session is a collision -> NULL (no overwrite). */
   turn_entry_t *dup = turn_registry_publish("sess-A", "turn-2");
   assert(dup == NULL);
   /* A different session is fine. */
   turn_entry_t *b = turn_registry_publish("sess-B", "turn-1");
   assert(b != NULL && b != a);
   turn_registry_clear(a);
   turn_registry_clear(b);
   /* After clear, the session can be published again. */
   turn_entry_t *a2 = turn_registry_publish("sess-A", "turn-3");
   assert(a2 != NULL);
   turn_registry_clear(a2);
   printf("ok: publish + collision\n");
}

/* Cross-principal cancel: the caller's ATTESTED principal must match the
 * session's presence owner. An attacker can declare any owner at attach time
 * but cannot authenticate as it, so this comparison is what keeps one attested
 * caller from cancelling another's session. handle_chat_graceful_cancel and
 * handle_chat_interrupt both render a -1 here as "forbidden: not the session
 * owner"; without the comparison those branches are unreachable. */
static void test_cancel_authz(void)
{
   turn_entry_t *e = turn_registry_publish("sess-D", "turn-1");
   assert(e);
   snprintf(g_stub_owner, sizeof(g_stub_owner), "webuser:alice");
   /* A different principal is refused (-1) and does NOT set the flag. */
   assert(turn_registry_cancel("sess-D", "webuser:bob") == -1);
   assert(turn_entry_cancelled(e) == 0);
   /* The matching principal succeeds. */
   assert(turn_registry_cancel("sess-D", "webuser:alice") == 1);
   assert(turn_entry_cancelled(e) == 1);
   /* A trusted-local caller (NULL principal) is exempt: it binds session id to
    * its authenticated caller before forwarding. */
   turn_entry_t *f = turn_registry_publish("sess-E", "turn-2");
   assert(f && turn_registry_cancel("sess-E", NULL) == 1);
   /* An unknown session is still 0, so the route can tell "nothing to cancel"
    * from "cancelled" rather than reporting success for a typo. */
   assert(turn_registry_cancel("sess-D-gone", "webuser:alice") == 0);
   g_stub_owner[0] = '\0';
   turn_registry_clear(e);
   turn_registry_clear(f);
   printf("ok: cancel authz (cross-principal cancel refused)\n");
}

static void test_cancel_and_find(void)
{
   turn_entry_t *e = turn_registry_publish("sess-C", "turn-1");
   assert(e);
   assert(turn_registry_find("sess-C") == e);
   assert(turn_entry_cancelled(e) == 0);
   int rc = turn_registry_cancel("sess-C", NULL); /* trusted internal cancel */
   assert(rc == 1);
   assert(turn_entry_cancelled(e) == 1);
   /* Cancelling a non-existent session returns 0. */
   assert(turn_registry_cancel("nope", NULL) == 0);
   turn_registry_clear(e);
   assert(turn_registry_find("sess-C") == NULL);
   printf("ok: cancel + find\n");
}

static void test_exact_turn_cancel(void)
{
   turn_entry_t *e = turn_registry_publish("exact-session", "current-turn");
   assert(e && turn_registry_cancel_exact("exact-session", "old-turn") == -1);
   assert(!turn_entry_cancelled(e));
   assert(turn_registry_cancel_exact("absent-session", "current-turn") == 0);
   assert(turn_registry_cancel_exact("exact-session", "current-turn") == 1);
   assert(turn_entry_cancelled(e));
   turn_registry_clear(e);
   e = turn_registry_publish("exact-session", "next-turn");
   assert(e && turn_registry_cancel_exact("exact-session", "current-turn") == -1);
   assert(!turn_entry_cancelled(e));
   turn_registry_clear(e);
}

static void test_cancel_all(void)
{
   turn_entry_t *e1 = turn_registry_publish("sess-E1", "t");
   turn_entry_t *e2 = turn_registry_publish("sess-E2", "t");
   assert(e1 && e2);
   int n = turn_registry_cancel_all();
   assert(n >= 2);
   assert(turn_entry_cancelled(e1) == 1);
   assert(turn_entry_cancelled(e2) == 1);
   turn_registry_clear(e1);
   turn_registry_clear(e2);
   printf("ok: cancel_all\n");
}

static void test_reaped(void)
{
   turn_entry_t *e = turn_registry_publish("sess-F", "t");
   assert(e);
   assert(e->reaped == 0);
   turn_registry_mark_reaped(e);
   assert(e->reaped == 1);
   turn_registry_clear(e);
   printf("ok: mark_reaped\n");
}

static cJSON *steer_message(const char *text)
{
   cJSON *message = cJSON_CreateObject(), *blocks = cJSON_AddArrayToObject(message, "blocks"),
         *block = cJSON_CreateObject();
   cJSON_AddStringToObject(message, "role", "user");
   cJSON_AddStringToObject(block, "type", "text");
   cJSON_AddStringToObject(block, "text", text);
   cJSON_AddItemToArray(blocks, block);
   return message;
}
static void test_steer_set_take(void)
{
   cJSON *msg = NULL, *first = steer_message("first"), *second = steer_message("second");
   assert(!chat_steer_take("sess-S", &msg) && !msg);
   assert(!chat_steer_set("sess-S", first));
   assert(chat_steer_take("sess-S", &msg) && cJSON_Compare(first, msg, 1));
   assert(msg != first);
   cJSON_Delete(msg);
   msg = NULL;
   assert(!chat_steer_take("sess-S", &msg));
   assert(!chat_steer_set("sess-S", first));
   assert(!chat_steer_set("sess-S", second));
   assert(chat_steer_take("sess-S", &msg) && cJSON_Compare(second, msg, 1));
   cJSON_Delete(msg);
   assert(chat_steer_set(NULL, first) == -1);
   assert(chat_steer_set("sess-S", NULL) == -1);
   cJSON *ma = NULL, *mb = NULL;
   chat_steer_set("sess-A", first);
   chat_steer_set("sess-B", second);
   assert(chat_steer_take("sess-A", &ma) && cJSON_Compare(first, ma, 1));
   assert(chat_steer_take("sess-B", &mb) && cJSON_Compare(second, mb, 1));
   cJSON_Delete(ma);
   cJSON_Delete(mb);
   chat_steer_set("sess-C", first);
   chat_steer_clear("sess-C");
   assert(!chat_steer_take("sess-C", &msg));
   cJSON_Delete(first);
   cJSON_Delete(second);
   printf("ok: canonical steer_set_take\n");
}

static char interrupt_error[128];
int handle_chat_interrupt(server_ctx_t *, server_conn_t *, cJSON *);
int server_send_error(server_conn_t *conn, const char *message, const char *request_id)
{
   (void)conn;
   (void)request_id;
   snprintf(interrupt_error, sizeof interrupt_error, "%s", message);
   return -1;
}
int server_send_response(server_conn_t *conn, cJSON *response)
{
   (void)conn;
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "interrupted")));
   return 0;
}
static void test_interrupt_privileged_webuser_stays_owner_bound(void)
{
   cJSON *request =
       cJSON_Parse("{\"aimee_session_id\":\"interrupt-owner\",\"message\":{\"role\":\"user\","
                   "\"blocks\":[{\"type\":\"text\",\"text\":\"steer exactly once\"}]}}");
   assert(request);
   snprintf(g_stub_owner, sizeof g_stub_owner, "webuser:alice");
   server_conn_t conn = {0};
   conn.capabilities = CAPS_ALL;
   snprintf(conn.vault_principal, sizeof conn.vault_principal, "webuser:bob");
   turn_entry_t *turn = turn_registry_publish("interrupt-owner", "turn");
   assert(turn);
   assert(handle_chat_interrupt(NULL, &conn, request) < 0);
   assert(strstr(interrupt_error, "not the session owner"));
   assert(!turn_entry_cancelled(turn));
   cJSON *queued = NULL;
   assert(!chat_steer_take("interrupt-owner", &queued));
   snprintf(conn.vault_principal, sizeof conn.vault_principal, "webuser:alice");
   assert(!handle_chat_interrupt(NULL, &conn, request));
   assert(turn_entry_cancelled(turn));
   assert(chat_steer_take("interrupt-owner", &queued));
   assert(cJSON_Compare(queued, cJSON_GetObjectItemCaseSensitive(request, "message"), 1));
   cJSON_Delete(queued);
   turn_registry_clear(turn);
   turn = turn_registry_publish("interrupt-owner", "admin-turn");
   assert(turn);
   conn.vault_principal[0] = 0;
   assert(!handle_chat_interrupt(NULL, &conn, request));
   assert(turn_entry_cancelled(turn));
   assert(chat_steer_take("interrupt-owner", &queued));
   cJSON_Delete(queued);
   turn_registry_clear(turn);
   cJSON_Delete(request);
   request = cJSON_CreateObject();
   assert(request);
   assert(cJSON_AddStringToObject(request, "aimee_session_id", "interrupt-owner"));
   const char *legacy = "literal text \"quoted\"\n雪 {\"role\":malformed}";
   assert(cJSON_AddStringToObject(request, "message", legacy));
   turn = turn_registry_publish("interrupt-owner", "legacy-turn");
   assert(turn);
   snprintf(conn.vault_principal, sizeof conn.vault_principal, "webuser:bob");
   assert(handle_chat_interrupt(NULL, &conn, request) < 0);
   assert(!turn_entry_cancelled(turn));
   assert(!chat_steer_take("interrupt-owner", &queued));
   assert(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(request, "message")));
   assert(cJSON_ReplaceItemInObjectCaseSensitive(request, "message", cJSON_CreateString(legacy)));
   snprintf(conn.vault_principal, sizeof conn.vault_principal, "webuser:alice");
   assert(!handle_chat_interrupt(NULL, &conn, request));
   assert(turn_entry_cancelled(turn));
   assert(chat_steer_take("interrupt-owner", &queued));
   aimee_message_t imported = {0};
   assert(!aimee_ir_message_parse(queued, &imported));
   assert(!strcmp(imported.role, "user") && imported.n_blocks == 1 &&
          imported.blocks[0].type == AIMEE_BLK_TEXT && !strcmp(imported.blocks[0].text, legacy));
   aimee_message_free(&imported);
   cJSON_Delete(queued);
   turn_registry_clear(turn);
   cJSON_Delete(request);
   printf(
       "ok: production interrupt binds privileged webuser to owner and imports public text once\n");
}

static void test_shutdown_fences_late_turns(void)
{
   turn_entry_t *e = turn_registry_publish("sess-shutdown-existing", "t");
   assert(e);
   assert(turn_registry_is_shutting_down() == 0);
   assert(turn_registry_begin_shutdown() >= 1);
   assert(turn_registry_is_shutting_down() == 1);
   assert(turn_entry_cancelled(e) == 1);
   assert(turn_registry_publish("sess-shutdown-late", "t") == NULL);
   turn_registry_clear(e);
   /* Clearing the final entry cannot reopen admission. */
   assert(turn_registry_publish("sess-shutdown-later", "t") == NULL);
   printf("ok: shutdown fences late turns\n");
}

int main(void)
{
   turn_registry_init();
   test_publish_and_collision();
   test_cancel_and_find();
   test_cancel_authz();
   test_exact_turn_cancel();
   test_cancel_all();
   test_reaped();
   test_steer_set_take();
   test_interrupt_privileged_webuser_stays_owner_bound();
   test_shutdown_fences_late_turns(); /* persistent fence: must be last */
   printf("all turn_registry tests passed\n");
   return 0;
}
