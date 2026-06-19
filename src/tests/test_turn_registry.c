/* test_turn_registry.c: unit tests for the per-turn cancel registry
 * (server-owned turn lifecycle, Phase 1). */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "turn_registry.h"

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

static void test_cancel_authz(void)
{
   turn_entry_t *e = turn_registry_publish("sess-D", "turn-1");
   assert(e);
   /* Owner of sess-D is "webuser:alice". */
   snprintf(g_stub_owner, sizeof(g_stub_owner), "webuser:alice");
   /* A different principal is refused (-1) and does NOT set the flag. */
   assert(turn_registry_cancel("sess-D", "webuser:bob") == -1);
   assert(turn_entry_cancelled(e) == 0);
   /* The matching principal succeeds. */
   assert(turn_registry_cancel("sess-D", "webuser:alice") == 1);
   assert(turn_entry_cancelled(e) == 1);
   g_stub_owner[0] = '\0';
   turn_registry_clear(e);
   printf("ok: cross-principal cancel rejected\n");
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

int main(void)
{
   turn_registry_init();
   test_publish_and_collision();
   test_cancel_and_find();
   test_cancel_authz();
   test_cancel_all();
   test_reaped();
   printf("all turn_registry tests passed\n");
   return 0;
}
