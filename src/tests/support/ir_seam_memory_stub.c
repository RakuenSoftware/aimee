/* ir_seam_memory_stub.c -- inert stubs for the memory module + config toggles that
 * aimee_ir_serve.c references once memory is registered on the IR transform seam
 * (aimee_ir_apply_request_stages). The minimal IR build/parity suites
 * (unit-test-aimee-ir-serve, unit-test-ir-legacy-parity) exercise the build +
 * translation paths, NOT memory injection, so they link this instead of the real
 * memory/config subsystem: the module is stubbed DISABLED (gw_stage_memory_enabled
 * -> 0) with an inert transform, and config_present() reports "no config" so
 * enablement falls to that env default. With memory off the seam is a no-op and every byte-exact
 * assertion in those suites is unchanged. Explicitly enabling the test seam counts
 * module-plan calls and records their query to verify the assembly boundary. */
#include <aimee/ir/module_plan.h>
#include "config.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static char g_test_session[80];
static char g_test_persona[1024];
static int g_test_delivery_state;
static int g_test_claim_error_once;
static int g_test_memory_enabled;
static int g_test_context_calls;
static int g_test_tools_calls;
static char g_test_query[1024];
static int g_test_guidance_provided;

int ir_seam_test_guidance_provided(void)
{
   return g_test_guidance_provided;
}

void ir_seam_test_memory(int enabled)
{
   g_test_memory_enabled = enabled;
   g_test_context_calls = g_test_tools_calls = 0;
   g_test_query[0] = '\0';
}

int ir_seam_test_plan_calls(const char *phase)
{
   return strcmp(phase, "context") == 0 ? g_test_context_calls : g_test_tools_calls;
}

const char *ir_seam_test_query(void)
{
   return g_test_query;
}

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

int aimee_ir_stage_module_plan(aimee_request_t *ir, void *ud)
{
   (void)ir;
   const aimee_ir_module_plan_t *plan = ud;
   if (strcmp(plan->phase, "context") == 0)
   {
      g_test_context_calls++;
      g_test_guidance_provided =
          plan->provided_resources && strcmp(plan->provided_resources[0], "guidance") == 0;
      snprintf(g_test_query, sizeof g_test_query, "%s",
               plan->provided_query ? plan->provided_query : "");
   }
   else if (strcmp(plan->phase, "tools") == 0)
      g_test_tools_calls++;
   return 0;
}

/* Keep the production persona transform on the shared IR seam.  Tests which do
 * not select a session/persona remain byte-identical; the ingress coverage below
 * can exercise real placement without linking the rest of the server. */

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

int server_ir_plan_enabled(const char *method, const char *operation, const char *value)
{
   return g_test_memory_enabled;
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

const aimee_ir_plan_binding_t server_ir_plan_bindings[] = {{NULL, NULL}};
const aimee_ir_plan_resource_t server_ir_plan_resources[] = {{NULL, NULL}};
