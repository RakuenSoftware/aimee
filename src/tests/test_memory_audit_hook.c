/* test_memory_audit_hook.c: the KB store-side memory-mutation audit hook. Pins
 * that memory_core_crud fires memory_set_audit_hook on insert / delete with the
 * NON-CONTENT identity (op, id, kind, key, tier, session) and NEVER the content,
 * and that a NULL hook is a no-op. This is the aimee-kb authoritative-record seam
 * (the KB bridge maps it onto aimee-kb's own obs_bus). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee.h" /* KIND_FACT, TIER_L2 */
#include "cJSON.h"
#include "db2_test_shim.h"
#include "kb/kb_memory_audit_bridge.h"
#include "log.h" /* audit_log_open */
#include "memory.h"
#include <aimee/audit/obs_bus.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

extern struct cJSON *audit_ledger_read(const char *from_ts, const char *to_ts);

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

   /* Re-inserting the SAME key with equivalent content overwrites the existing
    * memory via the exact-key MERGE path — a content mutation that MUST be audited
    * ("memory.merge"), not silently dropped. Its identity is fingerprinted like an
    * insert (no content). (Identical content avoids the supersede path, which a
    * materially-different re-store would take.) */
   memset(&g_last, 0, sizeof g_last);
   memory_t mm;
   assert(memory_insert(TIER_L2, KIND_FACT, "topic:release", secret_content, 0.9, "sess-2", &mm) ==
          0);
   assert(g_last.calls == 1);
   assert(strcmp(g_last.op, "memory.merge") == 0);
   assert(g_last.id == m.id); /* merged into the existing memory */
   assert(!strstr(g_last.key, "SECRET-CONTENT") && strstr(g_last.key, "topic") != NULL);

   /* Update: fires once, id-only. */
   memset(&g_last, 0, sizeof g_last);
   assert(memory_update_content(m.id, "revised content") == 0);
   assert(g_last.calls == 1 && strcmp(g_last.op, "memory.update") == 0 && g_last.id == m.id);

   /* Reject: fires once, id-only. */
   memset(&g_last, 0, sizeof g_last);
   assert(memory_reject(m.id, "low signal") == 0);
   assert(g_last.calls == 1 && strcmp(g_last.op, "memory.reject") == 0 && g_last.id == m.id);

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

   /* END TO END through the REAL aimee-kb bridge -> aimee-kb's obs_bus -> the KB
    * audit ledger: proves the store-side wiring (kb_memory_audit_bridge_install +
    * on_memory_mutation field mapping) and that the fingerprinted, content-free
    * row actually lands in the ledger — not just that the hook fires. */
   {
      char home[256];
      snprintf(home, sizeof home, "%s/aimee-kbmem-XXXXXX", platform_tmpdir());
      assert(mkdtemp(home));
      setenv("AIMEE_HOME", home, 1);
      audit_log_open();
      kb_memory_audit_bridge_install();

      memory_t e;
      assert(memory_insert(TIER_L2, KIND_FACT, "topic:e2e", "some content", 0.7, "sess-E", &e) ==
             0);
      obs_bus_stop(); /* drain to the KB ledger */

      cJSON *rows = audit_ledger_read(NULL, NULL);
      assert(rows);
      cJSON *r = NULL, *found = NULL;
      cJSON_ArrayForEach(r, rows)
      {
         cJSON *t = cJSON_GetObjectItemCaseSensitive(r, "tool");
         cJSON *tid = cJSON_GetObjectItemCaseSensitive(r, "task_id");
         if (cJSON_IsString(t) && strcmp(t->valuestring, "memory.insert") == 0 &&
             cJSON_IsNumber(tid) && (int64_t)tid->valuedouble == e.id)
         {
            found = r;
            break;
         }
      }
      assert(found);
      const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(found, "command"));
      assert(cmd && strncmp(cmd, "mk:", 3) == 0); /* identity fingerprinted, not raw */
      char *dump = cJSON_PrintUnformatted(rows);
      assert(dump && !strstr(dump, "topic:e2e")); /* raw key never in the ledger */
      free(dump);
      cJSON_Delete(rows);
   }

   printf("test_memory_audit_hook: OK (memory_core_crud fires the hook + end-to-end to the KB "
          "ledger; identity fingerprinted, no content)\n");
   return 0;
}
