/* wfe_iface.c -- the narrow executor vtable + step-result constructors. */
#include "wfe_iface.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- autonomous merge-target rail (WP-5 safety; see wfe_iface.h) ---- */

const char *wfe_autonomous_base(void)
{
   const char *b = getenv("AIMEE_AUTONOMY_BASE");
   return (b && b[0]) ? b : "testing";
}

int wfe_base_is_protected(const char *branch)
{
   if (!branch || !branch[0])
      return 1; /* empty -> treat as protected (fail closed) */
   if (strcmp(branch, "main") == 0 || strcmp(branch, "master") == 0)
      return 1;
   if (strncmp(branch, "release", 7) == 0)
      return 1;
   return 0;
}

int wfe_autonomous_target_ok(void)
{
   return !wfe_base_is_protected(wfe_autonomous_base());
}

static wfe_block_exec_fn g_execs[WFE_BLK__COUNT];

void wfe_register_block_executor(wfe_block_type_t type, wfe_block_exec_fn fn)
{
   if (type > WFE_BLK_UNKNOWN && type < WFE_BLK__COUNT)
      g_execs[type] = fn;
}

wfe_block_exec_fn wfe_lookup_block_executor(wfe_block_type_t type)
{
   if (type > WFE_BLK_UNKNOWN && type < WFE_BLK__COUNT)
      return g_execs[type];
   return NULL;
}

void wfe_reset_block_executors(void)
{
   memset(g_execs, 0, sizeof g_execs);
}

wfe_step_result_t wfe_step_advanced(const char *artifact_handle, const char *content_hash,
                                    double cost_usd)
{
   wfe_step_result_t r;
   memset(&r, 0, sizeof r);
   r.status = WFE_STEP_ADVANCED;
   if (artifact_handle)
      snprintf(r.artifact_handle, sizeof r.artifact_handle, "%s", artifact_handle);
   if (content_hash)
      snprintf(r.content_hash, sizeof r.content_hash, "%s", content_hash);
   r.cost_usd = cost_usd;
   return r;
}

wfe_step_result_t wfe_step_pending(wfe_pause_reason_t reason)
{
   wfe_step_result_t r;
   memset(&r, 0, sizeof r);
   r.status = WFE_STEP_PENDING;
   r.pause_reason = reason;
   return r;
}

wfe_step_result_t wfe_step_failed(void)
{
   wfe_step_result_t r;
   memset(&r, 0, sizeof r);
   r.status = WFE_STEP_FAILED;
   return r;
}

wfe_step_result_t wfe_step_looped(void)
{
   wfe_step_result_t r;
   memset(&r, 0, sizeof r);
   r.status = WFE_STEP_LOOPED;
   return r;
}
