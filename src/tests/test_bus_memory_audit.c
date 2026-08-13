/* test_bus_memory_audit.c: the server-side memory-mutation audit trail, END TO
 * END through the REAL bridge (memory_audit_bridge.c) onto the observability bus
 * and into the ledger.
 *
 * The server requests memory changes via kb_client (insert/update/delete/reject);
 * each fires kb_client_memory_audit_note, which this test drives directly (the
 * seam — no live aimee-kb needed). It installs the real bridge, notes a few
 * mutations, drains the async bus, then reads the ledger back and asserts each
 * row's actor/tool/command/mode/verdict/task_id. The memory CONTENT never enters
 * the hook (the signature has no content field), so it cannot reach the ledger;
 * this test also pins that no distinctive content-like value appears. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <aimee/audit/audit_action.h> /* audit_ensure_key */
#include "cJSON.h"
#include "kb_client.h" /* kb_client_memory_audit_note */
#include "log.h"       /* audit_log_open */
#include <aimee/audit/obs_bus.h>
#include "server/memory_audit_bridge.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* audit_ledger.h is not on the default include path from tests/ the same way;
 * declare the one function we use. */
extern struct cJSON *audit_ledger_read(const char *from_ts, const char *to_ts);

static const char *sval(cJSON *row, const char *key)
{
   const char *v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, key));
   return v ? v : "";
}

static double nval(cJSON *row, const char *key)
{
   cJSON *j = cJSON_GetObjectItemCaseSensitive(row, key);
   return cJSON_IsNumber(j) ? j->valuedouble : -1;
}

static cJSON *find_row(cJSON *rows, const char *tool, int64_t task_id)
{
   cJSON *r = NULL;
   cJSON_ArrayForEach(r, rows)
   {
      if (strcmp(sval(r, "tool"), tool) == 0 && (int64_t)nval(r, "task_id") == task_id)
         return r;
   }
   return NULL;
}

int main(void)
{
   printf("test_bus_memory_audit:\n");

   char home[256];
   snprintf(home, sizeof home, "%s/aimee-busmem-XXXXXX", platform_tmpdir());
   if (!mkdtemp(home))
   {
      fprintf(stderr, "FAIL: tmp home\n");
      return 1;
   }
   setenv("AIMEE_HOME", home, 1);
   audit_log_open();
   audit_ensure_key();

   /* Directly pin the shared PII fingerprint (used by BOTH the server and aimee-kb
    * memory bridges): "mk:" prefix, never the raw PII identity, deterministic, and
    * distinct for distinct identities. */
   {
      char fp1[32], fp2[32], fp3[32];
      obs_bus_key_fingerprint("fact", "email:alice@example.com", fp1, sizeof fp1);
      obs_bus_key_fingerprint("fact", "email:alice@example.com", fp2, sizeof fp2);
      obs_bus_key_fingerprint("fact", "email:bob@example.com", fp3, sizeof fp3);
      assert(strncmp(fp1, "mk:", 3) == 0);
      assert(strstr(fp1, "alice") == NULL && strstr(fp1, "email") == NULL &&
             strstr(fp1, "fact") == NULL);
      assert(strcmp(fp1, fp2) == 0); /* deterministic — correlatable */
      assert(strcmp(fp1, fp3) != 0); /* distinct identities distinguishable */
   }

   /* Install the REAL bridge: kb_client memory note -> hook -> obs_bus -> ledger. */
   memory_audit_bridge_install();

   /* A memory KEY carrying PII (the KB gates content but not the key): it must be
    * FINGERPRINTED, never surfaced verbatim, in the ledger. This is the load-
    * bearing assertion — a naive "kind/key" mapping would leak this. */
   const char *pii_key = "email:PIILEAK-alice@example.com";

   kb_client_memory_audit_note("memory.insert", 101, "L2", "fact", pii_key, 0.88, "sess-A", 1);
   kb_client_memory_audit_note("memory.update", 101, NULL, NULL, NULL, 0.0, NULL, 1);
   kb_client_memory_audit_note("memory.delete", 202, NULL, NULL, NULL, 0.0, NULL, 1);
   kb_client_memory_audit_note("memory.reject", 303, NULL, NULL, NULL, 0.0, NULL, 0); /* failed */
   kb_client_memory_audit_note("memory.supersede", 404, NULL, NULL, NULL, 0.9, "sess-A", 1);
   kb_client_memory_audit_note("memory.insert", 505, "L2", "fact", "k2", 0.5, "sess-A",
                               0); /* fail */

   obs_bus_stop(); /* drain to the ledger */

   cJSON *rows = audit_ledger_read(NULL, NULL);
   assert(rows);

   /* Insert: actor=session, mode=tier, verdict ok, task_id = memory id, and the
    * (kind, key) identity is a "mk:" FINGERPRINT — the raw PII key never appears. */
   cJSON *ins = find_row(rows, "memory.insert", 101);
   assert(ins);
   assert(strcmp(sval(ins, "actor"), "sess-A") == 0);
   assert(strncmp(sval(ins, "command"), "mk:", 3) == 0);
   assert(strstr(sval(ins, "command"), "PIILEAK") == NULL);
   assert(strstr(sval(ins, "command"), "alice") == NULL);
   assert(strstr(sval(ins, "command"), "fact") == NULL);
   assert(strcmp(sval(ins, "mode"), "L2") == 0);
   assert(strcmp(sval(ins, "verdict"), "ok") == 0);
   assert(strcmp(sval(ins, "reason_code"), "conf=0.88") == 0);

   /* Update: id-only op — no identity, actor defaults to "memory". */
   cJSON *upd = find_row(rows, "memory.update", 101);
   assert(upd);
   assert(strcmp(sval(upd, "actor"), "memory") == 0);
   assert(sval(upd, "command")[0] == '\0');
   assert(strcmp(sval(upd, "verdict"), "ok") == 0);

   /* Delete. */
   cJSON *del = find_row(rows, "memory.delete", 202);
   assert(del && strcmp(sval(del, "verdict"), "ok") == 0);

   /* Reject that FAILED -> verdict "fail". */
   cJSON *rej = find_row(rows, "memory.reject", 303);
   assert(rej && strcmp(sval(rej, "verdict"), "fail") == 0);

   /* Supersede is audited (id-only). */
   cJSON *sup = find_row(rows, "memory.supersede", 404);
   assert(sup && strcmp(sval(sup, "verdict"), "ok") == 0);

   /* A kb-REJECTED insert is recorded too (ok=0 -> verdict "fail"), symmetric with
    * the other verbs — the trail does not silently drop rejected stores. */
   cJSON *insf = find_row(rows, "memory.insert", 505);
   assert(insf && strcmp(sval(insf, "verdict"), "fail") == 0);

   /* THE invariant: no PII (from the key) and no content reach ANY field of ANY
    * row — the whole ledger dump is free of the plaintext key. */
   char *dump = cJSON_PrintUnformatted(rows);
   assert(dump);
   assert(!strstr(dump, "PIILEAK"));
   assert(!strstr(dump, "alice@example.com"));
   free(dump);

   cJSON_Delete(rows);
   printf("test_bus_memory_audit: OK (memory mutation -> bridge -> bus -> ledger; identity "
          "recorded, no content)\n");
   return 0;
}
