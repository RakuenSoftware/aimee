/* ir_seam_memory_stub.c -- inert stubs for the memory module + config toggles that
 * aimee_ir_serve.c references once memory is registered on the IR transform seam
 * (aimee_ir_apply_request_stages). The minimal IR build/parity suites
 * (unit-test-aimee-ir-serve, unit-test-ir-legacy-parity) exercise the build +
 * translation paths, NOT memory injection, so they link this instead of the real
 * memory/config subsystem: the module is stubbed DISABLED (gw_stage_memory_enabled
 * -> 0) with an inert transform, and config_present() reports "no config" so
 * enablement falls to that env default. With memory off the seam is a no-op and every byte-exact
 * assertion in those suites is unchanged. */
#include <aimee/ir/aimee_ir.h>
#include "config.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static char g_test_session[80];
static char g_test_persona[1024];
static int g_test_delivery_state;
static int g_test_claim_error_once;

void ir_seam_test_session(const char *session_id)
{
   snprintf(g_test_session, sizeof g_test_session, "%s", session_id ? session_id : "");
   g_test_delivery_state = 0;
   g_test_claim_error_once = 0;
}

void ir_seam_test_persona(const char *instructions)
{
   snprintf(g_test_persona, sizeof g_test_persona, "%s", instructions ? instructions : "");
}

void ir_seam_test_claim_error_once(void)
{
   g_test_claim_error_once = 1;
}

int ir_stage_memory(aimee_request_t *ir, void *ud)
{
   (void)ir;
   (void)ud;
   return 0;
}

/* Keep the production persona transform on the shared IR seam.  Tests which do
 * not select a session/persona remain byte-identical; the ingress coverage below
 * can exercise real placement without linking the rest of the server. */
int ir_stage_persona_instructions(aimee_request_t *ir, void *ud)
{
   return aimee_ir_prepend_persona_instructions(ir, (const char *)ud);
}

const char *server_http_identity_session_hdr(void)
{
   return g_test_session;
}

int session_persona_delivery_claim(const char *session_id)
{
   (void)session_id;
   if (g_test_claim_error_once)
   {
      g_test_claim_error_once = 0;
      return -1;
   }
   if (g_test_delivery_state)
      return 0;
   g_test_delivery_state = 2;
   return 1;
}

void session_persona_delivery_finish(const char *session_id, int delivered)
{
   (void)session_id;
   g_test_delivery_state = delivered ? 1 : 0;
}

int session_persona_get(const char *session_id, char *out, size_t n)
{
   (void)session_id;
   if (out && n)
      out[0] = '\0';
   return 0;
}

void config_current_persona(char *out, size_t n)
{
   if (out && n)
      snprintf(out, n, "%s", "engineer");
}

const char *config_default_persona(void)
{
   return "";
}

char *persona_compose_primary_instructions(const char *name, const char *cwd)
{
   (void)name;
   (void)cwd;
   return g_test_persona[0] ? strdup(g_test_persona) : NULL;
}

int gw_stage_memory_enabled(void)
{
   return 0;
}

/* aimee_ir_serve.c now asks config_present() + config_module_memory() instead of
 * loading a legacy_config_record and reading the field. Reporting "not present" reproduces
 * what the old legacy_config_read -> -1 stub produced: an unresolved tristate, so
 * enablement falls to the env default above. */
int config_present(void)
{
   return 0;
}

int config_module_memory(void)
{
   return -1;
}

int config_module_enabled(int config_tristate, int env_default)
{
   return config_tristate >= 0 ? config_tristate : env_default;
}

/* The IR stage runner now records per-turn session observations; this test
 * links the runner without the request-context object, so absorb the call. */
void request_context_note_aimee_session(int tool_calls, int redundant_tool_calls,
                                        const char *intervention, const char *tool_transport)
{
   (void)tool_calls;
   (void)redundant_tool_calls;
   (void)intervention;
   (void)tool_transport;
}

/* The first-turn shell block is registered on the same seam, so these suites link
 * it too. Inert here for the same reason as ir_stage_memory: they assert on the
 * BUILD and TRANSLATION bytes, and a stage that withheld a tool would change the
 * tools array they compare. Returning 0 keeps every byte-exact parity assertion
 * measuring what it was written to measure. */
int ir_stage_first_turn_shell_block(aimee_request_t *ir, void *ud)
{
   (void)ir;
   (void)ud;
   return 0;
}
