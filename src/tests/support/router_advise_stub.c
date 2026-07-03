/* router_advise_stub.c -- trivial router_autonomous_* implementations for tests
 * that link server_http_routes.o (rh_dev_submit references the S4 autonomous
 * router) but do not exercise the autonomous-routing path. The real logic lives
 * in router_advise.c and is covered by unit-test-wfe-autonomous-route (pure
 * clamp/floor) + the live CT smoke; here we only need the symbols to resolve
 * without pulling wfe_router + the catalog loader + DB1 into the link. The
 * autonomous router is default-OFF, so these stubs are never called by the
 * route tests -- they exist purely to satisfy the linker. */
#include "router_advise.h"

#include <stdio.h>

void router_autonomous_pick(const char *text, char *out_wf, size_t wf_n, char *out_src,
                            size_t src_n, char *out_raw, size_t raw_n, char *out_tag, size_t tag_n,
                            int *out_clamped)
{
   (void)text;
   if (out_wf && wf_n)
      snprintf(out_wf, wf_n, "managed-change");
   if (out_src && src_n)
      out_src[0] = '\0';
   if (out_raw && raw_n)
      out_raw[0] = '\0';
   if (out_tag && tag_n)
      out_tag[0] = '\0';
   if (out_clamped)
      *out_clamped = 1;
}

void router_autonomous_audit(const char *work_item_id, const char *chosen, const char *src,
                             const char *raw, int clamped, const char *tag)
{
   (void)work_item_id;
   (void)chosen;
   (void)src;
   (void)raw;
   (void)clamped;
   (void)tag;
}
