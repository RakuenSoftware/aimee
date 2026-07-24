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

#include "audit_action.h" /* audit_ensure_key */
#include "cJSON.h"
#include "kb_client.h" /* kb_client_memory_audit_note */
#include "log.h"       /* audit_log_open */
#include "modules/audit/obs_bus.h"
#include "server/memory_audit_bridge.h"

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

   char home[] = "/tmp/aimee-busmem-XXXXXX";
   if (!mkdtemp(home))
   {
      fprintf(stderr, "FAIL: tmp home\n");
      return 1;
   }
   setenv("AIMEE_HOME", home, 1);
   audit_log_open();
   audit_ensure_key();

   /* Install the REAL bridge: kb_client memory note -> hook -> obs_bus -> ledger. */
   memory_audit_bridge_install();

   /* A distinctive content-like string we NEVER pass to the note — it must not
    * appear anywhere in the ledger (the hook carries no content by construction). */
   const char *content_marker = "MEMORY-CONTENT-DO-NOT-LOG-7f2a";

   kb_client_memory_audit_note("memory.insert", 101, "L2", "fact", "ReleasePlan", 0.88, "sess-A",
                               1);
   kb_client_memory_audit_note("memory.update", 101, NULL, NULL, NULL, 0.0, NULL, 1);
   kb_client_memory_audit_note("memory.delete", 202, NULL, NULL, NULL, 0.0, NULL, 1);
   kb_client_memory_audit_note("memory.reject", 303, NULL, NULL, NULL, 0.0, NULL, 0); /* failed */

   obs_bus_stop(); /* drain to the ledger */

   cJSON *rows = audit_ledger_read(NULL, NULL);
   assert(rows);

   /* Insert: actor=session, command="kind/key" identity, mode=tier, verdict ok,
    * task_id = the memory id. */
   cJSON *ins = find_row(rows, "memory.insert", 101);
   assert(ins);
   assert(strcmp(sval(ins, "actor"), "sess-A") == 0);
   assert(strcmp(sval(ins, "command"), "fact/ReleasePlan") == 0);
   assert(strcmp(sval(ins, "mode"), "L2") == 0);
   assert(strcmp(sval(ins, "verdict"), "ok") == 0);
   assert(strcmp(sval(ins, "reason_code"), "conf=0.88") == 0);

   /* Update: id-only op — no kind/key identity, actor defaults to "memory". */
   cJSON *upd = find_row(rows, "memory.update", 101);
   assert(upd);
   assert(strcmp(sval(upd, "actor"), "memory") == 0);
   assert(sval(upd, "command")[0] == '\0');
   assert(strcmp(sval(upd, "verdict"), "ok") == 0);

   /* Delete. */
   cJSON *del = find_row(rows, "memory.delete", 202);
   assert(del && strcmp(sval(del, "verdict"), "ok") == 0);

   /* Reject that FAILED (kb rejected) -> verdict "fail". */
   cJSON *rej = find_row(rows, "memory.reject", 303);
   assert(rej && strcmp(sval(rej, "verdict"), "fail") == 0);

   /* THE invariant: memory content never reaches the trail. */
   char *dump = cJSON_PrintUnformatted(rows);
   assert(dump);
   assert(!strstr(dump, content_marker));
   free(dump);

   cJSON_Delete(rows);
   printf("test_bus_memory_audit: OK (memory mutation -> bridge -> bus -> ledger; identity "
          "recorded, no content)\n");
   return 0;
}
