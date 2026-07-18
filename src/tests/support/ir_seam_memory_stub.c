/* ir_seam_memory_stub.c -- inert stubs for the memory module + config toggles that
 * aimee_ir_serve.c references once memory is registered on the IR transform seam
 * (aimee_ir_apply_request_stages). The minimal IR build/parity suites
 * (unit-test-aimee-ir-serve, unit-test-ir-legacy-parity) exercise the build +
 * translation paths, NOT memory injection, so they link this instead of the real
 * memory/config subsystem: the module is stubbed DISABLED (gw_stage_memory_enabled
 * -> 0) with an inert transform, and config_load reports "no config" so enablement
 * falls to that env default. With memory off the seam is a no-op and every byte-exact
 * assertion in those suites is unchanged. */
#include "aimee_ir.h"
#include "config.h"
#include <string.h>

int ir_stage_memory(aimee_request_t *ir, void *ud)
{
   (void)ir;
   (void)ud;
   return 0;
}

int gw_stage_memory_enabled(void)
{
   return 0;
}

int config_load(config_t *c)
{
   if (c)
      memset(c, 0, sizeof *c);
   return -1;
}

int config_module_enabled(int config_tristate, int env_default)
{
   return config_tristate >= 0 ? config_tristate : env_default;
}
