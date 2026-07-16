/* ir_shadow_stubs.c -- WEAK no-op stubs for the response-side shadow hook wired
 * into agent_runtime.c. Tests that link agent_runtime.o to exercise the delegate
 * turn loop do not test the IR shadow, so they don't need the real
 * aimee_ir_shadow.o (which pulls in the whole backend/frontend/IR chain). These
 * stubs are WEAK: the real object's strong definitions win in the full binary and
 * in the shadow's own tests. With the stub, shadow is simply disabled (enabled ->
 * 0), so compare_response is never called by the runtime anyway. */
#include "aimee_ir_shadow.h"

__attribute__((weak)) int aimee_ir_shadow_enabled(void)
{
   return 0;
}

__attribute__((weak)) void aimee_ir_shadow_compare_response(const struct parsed_response *legacy,
                                                            const struct cJSON *resp_json,
                                                            aimee_wire_t wire)
{
   (void)legacy;
   (void)resp_json;
   (void)wire;
}
