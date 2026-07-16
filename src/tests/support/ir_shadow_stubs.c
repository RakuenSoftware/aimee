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

/* --- IR response-path symbols wired into agent_runtime.c ------------------
 * Weak so the minimal-link agent tests that don't exercise the IR response path
 * resolve without the whole backend/IR chain. The backend parse stubs return -1
 * (IR "could not parse"), which leaves the delegate turn loop with an empty parsed
 * response -- fine for tests that don't assert on it. Real objects win when linked. */
#include "aimee_backend.h"
#include "aimee_ir.h"

__attribute__((weak)) int anthropic_backend_parse(const struct cJSON *resp, aimee_response_t *out,
                                                  char *err, size_t errn)
{
   (void)resp;
   (void)out;
   (void)err;
   (void)errn;
   return -1;
}
__attribute__((weak)) int openai_backend_parse(const struct cJSON *resp, aimee_response_t *out,
                                               char *err, size_t errn)
{
   (void)resp;
   (void)out;
   (void)err;
   (void)errn;
   return -1;
}
__attribute__((weak)) size_t aimee_ir_response_text(const aimee_response_t *r, char *buf, size_t n)
{
   (void)r;
   if (buf && n)
      buf[0] = '\0';
   return 0;
}
__attribute__((weak)) void aimee_response_free(aimee_response_t *r)
{
   (void)r;
}
