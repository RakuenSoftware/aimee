/* test_memory_audit_hook.c: the KB store-side memory-mutation audit hook. Pins
 * that memory_core_crud fires memory_set_audit_hook on insert / delete with the
 * NON-CONTENT identity (op, id, kind, key, tier, session) and NEVER the content,
 * and that a NULL hook is a no-op. This is the aimee-kb authoritative-record seam
 * (the KB bridge maps it onto aimee-kb's own obs_bus). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h" /* KIND_FACT, TIER_L2 */
#include "db2_test_shim.h"
#include "memory.h"

static struct
{
   int calls;
   char op[32];
   int64_t id;
   char tier[16];
   char kind[32];
   char key[96];
   char session[32];
} g_last;

static void capture(const char *op, int64_t id, const char *tier, const char *kind, const char *key,
                    double confidence, const char *session_id)
{
   (void)confidence;
   g_last.calls++;
   snprintf(g_last.op, sizeof g_last.op, "%s", op);
   g_last.id = id;
   snprintf(g_last.tier, sizeof g_last.tier, "%s", tier);
   snprintf(g_last.kind, sizeof g_last.kind, "%s", kind);
   snprintf(g_last.key, sizeof g_last.key, "%s", key);
   snprintf(g_last.session, sizeof g_last.session, "%s", session_id);
}

int main(void)
{
   db2_test_shim_open();
   memory_set_audit_hook(capture);

   /* Insert: the hook fires once with the identity — and NOT the content. */
   const char *secret_content = "SECRET-CONTENT-DO-NOT-LOG-alice@example.com";
   memory_t m;
   memset(&g_last, 0, sizeof g_last);
   assert(memory_insert(TIER_L2, KIND_FACT, "topic:release", secret_content, 0.8, "sess-1", &m) ==
          0);
   assert(g_last.calls == 1);
   assert(strcmp(g_last.op, "memory.insert") == 0);
   assert(g_last.id == m.id && m.id > 0);
   assert(strcmp(g_last.kind, KIND_FACT) == 0);
   assert(strstr(g_last.key, "topic") != NULL); /* the (normalized) key identity */
   assert(strcmp(g_last.session, "sess-1") == 0);
   /* The content — the PII payload — never reaches the hook. */
   assert(!strstr(g_last.key, "SECRET-CONTENT") && !strstr(g_last.key, "alice"));
   assert(!strstr(g_last.op, "SECRET") && !strstr(g_last.kind, "SECRET") &&
          !strstr(g_last.session, "SECRET"));

   /* Delete: the hook fires once, id-only. */
   memset(&g_last, 0, sizeof g_last);
   assert(memory_delete(m.id) == 0);
   assert(g_last.calls == 1);
   assert(strcmp(g_last.op, "memory.delete") == 0);
   assert(g_last.id == m.id);

   /* A NULL hook records nothing (no crash). */
   memory_set_audit_hook(NULL);
   memset(&g_last, 0, sizeof g_last);
   memory_t m2;
   assert(memory_insert(TIER_L2, KIND_FACT, "topic:other", "content", 0.8, "sess-1", &m2) == 0);
   assert(g_last.calls == 0);

   printf("test_memory_audit_hook: OK (memory_core_crud fires the hook; identity, no content)\n");
   return 0;
}
