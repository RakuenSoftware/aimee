/* test_bus_tool_completion.c: the tool-call COMPLETION audit trail, through the
 * REAL server bridge (tool_completion_audit_bridge.c) onto the audit event bus
 * and into the ledger.
 *
 * The dispatcher reduces every tool call to a fixed verdict / reason_code / mode
 * enum before firing the completion hook, so this pins the OTHER half — the
 * bridge's field mapping and the bus->ledger path — by driving the hook through
 * the dependency-free completion TU's test seam (linking the whole dispatcher
 * here would pull the entire agent). It asserts:
 *   - one row per completed call, with the outcome's verdict / mode / reason_code
 *     and the captured actor;
 *   - the row is fingerprint/identity only — a "v1-" name-only args_hash, an empty
 *     command, and NO field that could carry argument, result, or error content;
 *   - every recorded reason_code is one of the fixed enum values (never free text
 *     such as an MCP server's err_buf), which is the property that keeps content
 *     off the bus and out of the 0600 capture file.
 *
 * The complementary end-to-end path (a REAL failing MCP call, asserting its
 * err_buf never reaches the row) is a heavier dispatcher-integration test.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <aimee/audit/audit_action.h> /* audit_ensure_key */
#include <aimee/audit/audit_ledger.h>
#include <aimee/audit/obs_bus.h>

#include "cJSON.h"
#include "config.h" /* header-order for agent_tools.h */
#include "log.h"    /* audit_log_open */

#include <aimee/tools/agent_tools.h> /* agent_tool_completion_t, agent_tools_fire_tool_completion_for_test */
#include "server/tool_completion_audit_bridge.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static const char *sval(cJSON *row, const char *key)
{
   const char *v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, key));
   return v ? v : "";
}

static cJSON *find_row(cJSON *rows, const char *tool)
{
   cJSON *r = NULL;
   cJSON_ArrayForEach(r, rows)
   {
      if (strcmp(sval(r, "tool"), tool) == 0)
         return r;
   }
   return NULL;
}

static int reason_is_known(const char *rc)
{
   static const char *ok[] = {"",        "guardrail",    "role",     "cancelled", "tool_error",
                              "timeout", "unknown_tool", "bad_args", "policy",    NULL};
   for (int i = 0; ok[i]; i++)
      if (strcmp(rc, ok[i]) == 0)
         return 1;
   return 0;
}

int main(void)
{
   printf("test_bus_tool_completion:\n");

   char home[256];
   snprintf(home, sizeof home, "%s/aimee-bustc-XXXXXX", platform_tmpdir());
   if (!mkdtemp(home))
   {
      fprintf(stderr, "FAIL: tmp home\n");
      return 1;
   }
   setenv("AIMEE_HOME", home, 1);
   audit_log_open();
   audit_ensure_key();

   /* Install the REAL bridge (sets the process-global completion hook). */
   tool_completion_audit_bridge_install();

   /* Drive the three security-relevant outcome shapes through the hook. */
   agent_tool_completion_t ok = {
       .actor = "sess-A", .verdict = "ok", .reason_code = "", .mode = "internal"};
   agent_tools_fire_tool_completion_for_test("read_file", &ok);

   agent_tool_completion_t refused = {
       .actor = "sess-A", .verdict = "refused", .reason_code = "guardrail", .mode = "internal"};
   agent_tools_fire_tool_completion_for_test("bash", &refused);

   agent_tool_completion_t mcperr = {
       .actor = "sess-B", .verdict = "error", .reason_code = "tool_error", .mode = "outbound:sse"};
   agent_tools_fire_tool_completion_for_test("github:create_issue", &mcperr);

   /* A served call (external client -> aimee's tool) refused at the gate, and a
    * local stdio MCP server call: the mode carries the transport / direction. */
   agent_tool_completion_t served = {
       .actor = "client-1", .verdict = "refused", .reason_code = "policy", .mode = "served"};
   agent_tools_fire_tool_completion_for_test("git", &served);

   agent_tool_completion_t stdio = {
       .actor = "sess-B", .verdict = "ok", .reason_code = "", .mode = "outbound:stdio"};
   agent_tools_fire_tool_completion_for_test("localfs:read", &stdio);

   /* A served tool name is attacker-influenceable identity. Feed one carrying raw
    * control bytes (a newline that could forge a row, an ANSI escape that could
    * injection a terminal on --audit-replay) around a printable marker; the emit
    * serializer must scrub the control bytes to '?' while keeping the marker. */
   const char *CTRL_TOOL = "evil\ntool\x1b[31mMARKER42";
   agent_tool_completion_t evil = {
       .actor = "client-x", .verdict = "ok", .reason_code = "", .mode = "served"};
   agent_tools_fire_tool_completion_for_test(CTRL_TOOL, &evil);

   obs_bus_stop(); /* drain the async bus to the ledger */

   cJSON *rows = audit_ledger_read(NULL, NULL);
   assert(rows);

   cJSON *r_ok = find_row(rows, "read_file");
   assert(r_ok);
   assert(strcmp(sval(r_ok, "actor"), "sess-A") == 0);
   assert(strcmp(sval(r_ok, "verdict"), "ok") == 0);
   assert(strcmp(sval(r_ok, "mode"), "internal") == 0);
   /* Identity only: a name-only fingerprint and no command line. */
   assert(strncmp(sval(r_ok, "args_hash"), "v1-", 3) == 0);
   assert(strcmp(sval(r_ok, "command"), "") == 0);

   cJSON *r_ref = find_row(rows, "bash");
   assert(r_ref);
   assert(strcmp(sval(r_ref, "verdict"), "refused") == 0);
   assert(strcmp(sval(r_ref, "reason_code"), "guardrail") == 0);

   cJSON *r_mcp = find_row(rows, "github:create_issue");
   assert(r_mcp);
   assert(strcmp(sval(r_mcp, "verdict"), "error") == 0);
   assert(strcmp(sval(r_mcp, "reason_code"), "tool_error") == 0);
   assert(strcmp(sval(r_mcp, "mode"), "outbound:sse") == 0);

   cJSON *r_srv = find_row(rows, "git");
   assert(r_srv);
   assert(strcmp(sval(r_srv, "mode"), "served") == 0);
   assert(strcmp(sval(r_srv, "verdict"), "refused") == 0);

   cJSON *r_std = find_row(rows, "localfs:read");
   assert(r_std);
   assert(strcmp(sval(r_std, "mode"), "outbound:stdio") == 0);

   /* Control bytes in the attacker-influenced tool name are scrubbed to '?': the
    * printable marker survives, but no raw newline / ESC reaches the ledger (which
    * feeds the same bytes to the capture file + --audit-replay). */
   cJSON *r_evil = NULL, *r = NULL;
   cJSON_ArrayForEach(r, rows) if (strstr(sval(r, "tool"), "MARKER42")) r_evil = r;
   assert(r_evil);
   const char *et = sval(r_evil, "tool");
   for (const char *p = et; *p; p++)
      assert((unsigned char)*p >= 0x20 && (unsigned char)*p != 0x7f); /* no control bytes */
   assert(strchr(et, '\n') == NULL && strchr(et, '\x1b') == NULL);

   /* Every recorded reason_code is a fixed enum value (never free text / err_buf). */
   cJSON_ArrayForEach(r, rows) assert(reason_is_known(sval(r, "reason_code")));

   printf("  completion rows: verdict/mode/reason recorded; identity-only; no content leak\n");
   cJSON_Delete(rows);
   printf("test_bus_tool_completion: OK\n");
   return 0;
}
