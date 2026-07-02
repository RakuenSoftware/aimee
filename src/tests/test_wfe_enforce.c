/* test_wfe_enforce.c -- S2 pure enforcement cores: deliver-primitive set,
 * per-block tool surface, surface allow-check (with the delivery gate), and the
 * rollout fail-class split. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "wfe_enforce.h"
#include "wfe_externalization.h"
#include "wfe_iface.h"

int main(void)
{
   printf("wfe-enforce: ");

   /* --- DELIVER_PRIMITIVES set --- */
   assert(wfe_is_deliver_primitive("pr.open") && wfe_is_deliver_primitive("merge"));
   assert(wfe_is_deliver_primitive("deploy") && wfe_is_deliver_primitive("release"));
   assert(wfe_is_deliver_primitive("mcp__github__create_pull_request")); /* substring */
   assert(!wfe_is_deliver_primitive("Read") && !wfe_is_deliver_primitive("Edit"));
   assert(!wfe_is_deliver_primitive("") && !wfe_is_deliver_primitive(NULL));

   /* --- write / delegate tool classification --- */
   assert(wfe_is_write_tool("Edit") && wfe_is_write_tool("Write") &&
          wfe_is_write_tool("apply_patch"));
   assert(!wfe_is_write_tool("Read") && !wfe_is_write_tool("Grep") && !wfe_is_write_tool(NULL));
   assert(wfe_is_delegate_tool("delegate") && wfe_is_delegate_tool("Task") &&
          wfe_is_delegate_tool("Subagent"));
   assert(!wfe_is_delegate_tool("Read") && !wfe_is_delegate_tool(NULL));

   /* --- per-block default surface --- */
   assert(wfe_block_default_surface(WFE_BLK_UNDERSTAND) == WFE_SURFACE_READONLY);
   assert(wfe_block_default_surface(WFE_BLK_REVIEW) == WFE_SURFACE_READONLY);
   assert(wfe_block_default_surface(WFE_BLK_GATE_DELIVER) == WFE_SURFACE_READONLY);
   assert(wfe_block_default_surface(WFE_BLK_IMPLEMENT) == WFE_SURFACE_DELEGATE);
   assert(wfe_block_default_surface(WFE_BLK_DOCUMENT) == WFE_SURFACE_DELEGATE);
   assert(wfe_block_default_surface(WFE_BLK_PR_OPEN) == WFE_SURFACE_FULL);

   /* --- surface allow-check --- */
   /* READONLY: reads ok; write/delegate denied; deliver gated on delivery */
   assert(wfe_surface_allows(WFE_SURFACE_READONLY, "Read", 0) == 1);
   assert(wfe_surface_allows(WFE_SURFACE_READONLY, "Edit", 0) == 0);
   assert(wfe_surface_allows(WFE_SURFACE_READONLY, "delegate", 0) == 0);
   assert(wfe_surface_allows(WFE_SURFACE_READONLY, "pr.open", 0) == 0);
   assert(wfe_surface_allows(WFE_SURFACE_READONLY, "pr.open", 1) == 1); /* delivered */
   assert(wfe_surface_allows(WFE_SURFACE_READONLY, NULL, 0) == 0);

   /* DELEGATE: delegate ok, direct write denied, deliver gated */
   assert(wfe_surface_allows(WFE_SURFACE_DELEGATE, "delegate", 0) == 1);
   assert(wfe_surface_allows(WFE_SURFACE_DELEGATE, "Read", 0) == 1);
   assert(wfe_surface_allows(WFE_SURFACE_DELEGATE, "Edit", 0) == 0);
   assert(wfe_surface_allows(WFE_SURFACE_DELEGATE, "merge", 0) == 0);

   /* FULL: write/delegate ok, but deliver STILL gated pre-delivery */
   assert(wfe_surface_allows(WFE_SURFACE_FULL, "Edit", 0) == 1);
   assert(wfe_surface_allows(WFE_SURFACE_FULL, "delegate", 0) == 1);
   assert(wfe_surface_allows(WFE_SURFACE_FULL, "pr.open", 0) == 0);
   assert(wfe_surface_allows(WFE_SURFACE_FULL, "pr.open", 1) == 1);

   /* --- rollout fail-class split --- */
   /* a policy denial always fails closed */
   assert(wfe_enforce_fail_action(WFE_FAIL_POLICY, 0, 0) == WFE_ACT_FAIL_CLOSED);
   assert(wfe_enforce_fail_action(WFE_FAIL_POLICY, 1, 1) == WFE_ACT_FAIL_CLOSED);
   /* instrumentation failure: chat fails open, but a deliver/write fails closed in hard */
   assert(wfe_enforce_fail_action(WFE_FAIL_INSTRUMENTATION, 1, 1) == WFE_ACT_FAIL_CLOSED);
   assert(wfe_enforce_fail_action(WFE_FAIL_INSTRUMENTATION, 1, 0) == WFE_ACT_FAIL_OPEN_CHAT);
   assert(wfe_enforce_fail_action(WFE_FAIL_INSTRUMENTATION, 0, 1) == WFE_ACT_FAIL_OPEN_CHAT);

   /* --- staged dial --- */
   assert(wfe_enforce_stage_parse(NULL) == WFE_ENFORCE_OFF);
   assert(wfe_enforce_stage_parse("off") == WFE_ENFORCE_OFF);
   assert(wfe_enforce_stage_parse("bogus") == WFE_ENFORCE_OFF); /* unknown -> off */
   assert(wfe_enforce_stage_parse("advisory") == WFE_ENFORCE_ADVISORY);
   assert(wfe_enforce_stage_parse("SOFT") == WFE_ENFORCE_SOFT); /* case-insensitive */
   assert(wfe_enforce_stage_parse("hard") == WFE_ENFORCE_HARD);
   assert(strcmp(wfe_enforce_stage_name(WFE_ENFORCE_HARD), "hard") == 0);
   assert(!wfe_enforce_stage_restricts(WFE_ENFORCE_OFF) &&
          !wfe_enforce_stage_restricts(WFE_ENFORCE_ADVISORY));
   assert(wfe_enforce_stage_restricts(WFE_ENFORCE_SOFT) &&
          wfe_enforce_stage_restricts(WFE_ENFORCE_HARD));
   assert(!wfe_enforce_stage_refuses(WFE_ENFORCE_SOFT) &&
          wfe_enforce_stage_refuses(WFE_ENFORCE_HARD));

   /* --- templated surfacing: names the gate + id, NEVER echoes injected content --- */
   char msg[256];
   wfe_enforce_user_message(WFE_ENFORCE_HARD, "deliver", "wi_abc123", msg, sizeof msg);
   assert(strstr(msg, "deliver") && strstr(msg, "wi_abc123") && strstr(msg, "blocked"));
   /* an injection-shaped gate/id is rejected -> safe fallback, no echo */
   wfe_enforce_user_message(WFE_ENFORCE_SOFT, "\" onclick=x", "id\";DROP", msg, sizeof msg);
   assert(!strstr(msg, "onclick") && !strstr(msg, "DROP") && strstr(msg, "workflow gate"));
   wfe_enforce_user_message(WFE_ENFORCE_SOFT, NULL, NULL, msg, sizeof msg); /* no crash */

   /* --- CAS advance guard --- */
   assert(wfe_advance_cas_ok("implement", "implement") == 1);  /* match -> advance */
   assert(wfe_advance_cas_ok("understand", "implement") == 0); /* stale -> reject */
   assert(wfe_advance_cas_ok(NULL, "implement") == -1 && wfe_advance_cas_ok("implement", "") == -1);

   /* --- sliding-lease TTL (step 6) --- */
   unsetenv("AIMEE_WORKFLOW_LEASE_TTL_SECS");
   assert(wfe_lease_ttl_secs() == 3600); /* default */
   setenv("AIMEE_WORKFLOW_LEASE_TTL_SECS", "120", 1);
   assert(wfe_lease_ttl_secs() == 120);
   setenv("AIMEE_WORKFLOW_LEASE_TTL_SECS", "0", 1);
   assert(wfe_lease_ttl_secs() == 0); /* disable */
   setenv("AIMEE_WORKFLOW_LEASE_TTL_SECS", "garbage", 1);
   assert(wfe_lease_ttl_secs() == 3600); /* bad value -> default */
   setenv("AIMEE_WORKFLOW_LEASE_TTL_SECS", "99999999", 1);
   assert(wfe_lease_ttl_secs() == 3600); /* out of range -> default */
   unsetenv("AIMEE_WORKFLOW_LEASE_TTL_SECS");

   printf("ok\n");
   return 0;
}
