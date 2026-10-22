/* aimee_ir_serve.c -- see aimee_ir_serve.h. */
#include "aimee_ir_serve.h"
#include "request_context.h"

/* Capture the original user text before persona insertion. The module receives
 * this immutable snapshot separately from the transformed transcript. */
#define IR_STAGE_QUERY_MAX 16384

#include <aimee/translation/aimee_backend.h>
#include <aimee/translation/aimee_frontend.h>
#include "ir_host_bindings.h"
#include "config.h" /* legacy_config_read + config_module_enabled (modules.memory) */
#include "persona.h"
#include "server_http.h"          /* session_persona_get */
#include "server_http_identity.h" /* inbound session identity */
#include "request_context.h"
#include <aimee/ir/aimee_ir_metrics.h>
#include "aimee.h" /* size macros for agent_types.h */
#include <aimee/ir/aimee_ir.h>
#include <aimee/core/turn_integrity.h>
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int aimee_ir_path_enabled(void)
{
   /* Retained ABI for callers; canonical admission has no legacy escape hatch. */
   return 1;
}

int aimee_ir_stream_relay_enabled(void)
{
   /* Kept for existing callers; ingress has no legacy stream execution path. */
   return 1;
}

/* Resolve the memory module toggle: config-store modules.memory (canonical) -> env
 * default parsed by the Go owner. Cached legacy_config_read applies operator
 * toggles without restarting. Resolved at the seam
 * call site, mirroring the legacy gw_stage_slot_t catalogs. */
static int ir_memory_enabled(void)
{
   int tri = config_present() ? config_module_memory() : -1;
   return config_module_enabled(tri, server_ir_plan_enabled("memory.runtime", "gateway-enabled",
                                                            getenv("AIMEE_STAGE_MEMORY")));
}

static int ir_stage_context_authority(aimee_request_t *ir, void *ud)
{
   (void)ud;
   /* Metadata only: classifying an existing block must not force a provider
    * payload rebuild. Newly inserted blocks are dirtied by their own stage. */
   (void)aimee_ir_classify_context(ir);
   return 0;
}

static int ir_stage_knowledge_freshness(aimee_request_t *ir, void *ud)
{
   (void)ud;
   const char *session = server_http_identity_session_hdr();
   if (!ir || !session || !session[0])
      return 0;
   uint64_t current = ti_knowledge_epoch_current("knowledge", "global");
   uint64_t previous = 0;
   if (ti_session_knowledge_observe(session, current, &previous) != TI_FRESHNESS_STALE)
      return 0;

   char marker[384];
   snprintf(marker, sizeof marker,
            "<aimee-freshness status=\"stale\" previous_epoch=\"%llu\" "
            "current_epoch=\"%llu\">Prior assistant factual claims may rely on older "
            "knowledge. Re-retrieve affected facts before relying on them.</aimee-freshness>",
            (unsigned long long)previous, (unsigned long long)current);
   aimee_block_t *grown = realloc(ir->system, (size_t)(ir->n_system + 1) * sizeof *grown);
   if (!grown)
      return 0;
   ir->system = grown;
   aimee_block_t *block = &ir->system[ir->n_system];
   memset(block, 0, sizeof *block);
   block->type = AIMEE_BLK_TEXT;
   block->text = strdup(marker);
   if (!block->text)
      return 0;
   aimee_ir_block_set_context(block, AIMEE_CTX_ORIGIN_PLATFORM, AIMEE_CTX_AUTH_TASK_INSTRUCTION,
                              AIMEE_CTX_TRUST_VERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, "knowledge",
                              "global", current);
   ir->n_system++;
   return 1;
}

static char *ir_resolve_persona_instructions(void)
{
   char name[PERSONA_NAME_MAX] = "";
   const char *sid = server_http_identity_session_hdr();
   if (!(sid && sid[0] && session_persona_get(sid, name, sizeof name)))
      config_current_persona(name, sizeof name);
   if (strcmp(name, "engineer") == 0 && config_default_persona()[0])
      snprintf(name, sizeof name, "%s", config_default_persona());
   return persona_compose_primary_instructions(name, NULL);
}

char *aimee_ir_prepend_active_persona_text(const char *text, int conversation_established)
{
   const char *sid = server_http_identity_session_hdr();
   int claim = session_persona_delivery_claim(sid);
   if (claim == 0)
      return strdup(text ? text : "");
   if (claim < 0)
   {
      fprintf(stderr, "aimee: persona delivery state unavailable; retaining retry eligibility\n");
      return NULL;
   }
   /* A migrated/restarted named session starts with delivery_state=0 even when
    * its transcript is already established. Commit that observation before a
    * later compacted request drops the assistant history; otherwise the legacy
    * flat path would misclassify the compacted turn as a new conversation and
    * inject a persona midway through it. */
   if (conversation_established)
   {
      char *out = strdup(text ? text : "");
      session_persona_delivery_finish(sid, out != NULL);
      return out;
   }
   char *instructions = ir_resolve_persona_instructions();
   char *out = aimee_ir_prepend_persona_text(text, instructions);
   session_persona_delivery_finish(sid, instructions && instructions[0] && out);
   free(instructions);
   return out;
}

/* A transform returning zero can still mean the persona was already present, or
 * that a restarted server first observed an established conversation.  Both are
 * terminal.  Only a genuine first request that could not be mutated (for example
 * allocation failure or no user message yet) should release the reservation. */
static int ir_persona_delivery_already_satisfied(const aimee_request_t *ir)
{
   if (!ir)
      return 0;
   for (int i = 0; i < ir->n_messages; i++)
   {
      const aimee_message_t *message = &ir->messages[i];
      if (message->role && strcmp(message->role, "assistant") == 0)
         return 1;
      if (!message->role || strcmp(message->role, "user") != 0)
         continue;
      for (int j = 0; j < message->n_blocks; j++)
         if (message->blocks[j].type == AIMEE_BLK_TEXT && message->blocks[j].text &&
             strstr(message->blocks[j].text, "<aimee-persona ") != NULL)
            return 1;
      return 0;
   }
   return 0;
}

void aimee_ir_apply_request_stages(aimee_request_t *ir, int memory_enabled)
{
   /* The one place modules register on the IR (universal-gateway P4): fired once per
    * ingress, after frontend_parse and before backend_build, protocol-neutral. Memory
    * is the first ported module -- it replaces gw_stage_memory's two structured-ingress
    * arms (anthropic /v1/messages + /v1/responses). The four legacy plain-chat handlers
    * stay on gw_memory_system_prompt until agent_execute itself moves onto the IR. */
   /* Capture the user's OWN query before anything is prepended to it.
    *
    * ir_stage_memory takes its query from aimee_ir_last_user_text(), and the
    * persona below is inserted onto that same first user message BEFORE the
    * stage list runs. So on any turn that delivers a persona -- the opening turn
    * of every session -- the "query" put to recall was thousands of characters
    * of persona with the real question buried at the end, which matches nothing.
    * Memory pre-injection was silently dead exactly when it matters most, with no
    * error anywhere: recall answers "no rows", the block assembles empty, and
    * ingress_preinject_build returns NULL before it ever reaches the confidence
    * call. Measured on the box: the same question recalls 1 row asked plainly and
    * 0 rows asked the way the stage asked it. */
   char *pristine_query = malloc(IR_STAGE_QUERY_MAX);
   if (pristine_query && aimee_ir_last_user_text(ir, pristine_query, IR_STAGE_QUERY_MAX) == 0)
   {
      free(pristine_query);
      pristine_query = NULL;
   }

   const char *sid = server_http_identity_session_hdr();
   int persona_first = session_persona_delivery_claim(sid);
   if (persona_first < 0)
      fprintf(stderr, "aimee: persona delivery state unavailable; retaining retry eligibility\n");
   char *persona_instructions = persona_first > 0 ? ir_resolve_persona_instructions() : NULL;
   int persona_inserted = 0;
   if (persona_instructions && persona_instructions[0])
      persona_inserted = aimee_ir_prepend_persona_instructions(ir, persona_instructions);
   free(persona_instructions);
   int persona_delivered = persona_inserted || ir_persona_delivery_already_satisfied(ir);
   if (persona_first > 0)
      session_persona_delivery_finish(sid, persona_delivered);
   /* The host-composed persona includes the standing guidance. Tell the module
    * only when this assembly actually inserted it; caller text is not proof. */
   const char *const persona_resources[] = {"guidance", NULL};
   aimee_ir_module_plan_t context_plan = {.method = "memory.runtime",
                                          .operation = "gateway-plan",
                                          .phase = "context",
                                          .provided_query = pristine_query,
                                          .provided_resources =
                                              persona_inserted ? persona_resources : NULL,
                                          .bindings = server_ir_plan_bindings,
                                          .refuse = server_ir_plan_refuse,
                                          .resources = server_ir_plan_resources};
   aimee_ir_module_plan_t tools_plan = context_plan;
   tools_plan.phase = "tools";
   const aimee_ir_transform_t stages[] = {
       {"context_authority", ir_stage_context_authority, NULL, 1},
       {"knowledge_freshness", ir_stage_knowledge_freshness, NULL, 1},
       {"memory", aimee_ir_stage_module_plan, &context_plan, memory_enabled},
       /* Runs AFTER memory, so the opening turn already carries the guidance that
        * names what replaces the shell it is about to lose. Always on: an agent
        * that never reaches aimee's tools is not using aimee. */
       {"first_turn_shell_block", aimee_ir_stage_module_plan, &tools_plan, 1},
       /* Reads the transcript the other stages just shaped, so it judges the turn
        * that will actually be sent rather than the one that arrived. */
       {"session_assist", aimee_ir_stage_session_assist, NULL, 1},
   };
   if (persona_inserted)
      ir->mutated = 1;
   aimee_ir_run_transforms(ir, stages, sizeof stages / sizeof stages[0]);
   free(pristine_query);

   /* What the turn actually did, recorded on the ingress request so the ordinary
    * token-audit row carries behaviour beside cost. Derived from the IR, so this
    * is one implementation for every client rather than one per wire format. */
   aimee_ir_session_metrics_t metrics;
   aimee_ir_session_measure(ir, &metrics);
   int shell = 0, mcp = 0;
   for (int i = 0; ir && i < ir->n_tools; i++)
   {
      const char *name = ir->tools[i].name;
      if (name && (strstr(name, "shell") || strstr(name, "exec_command") ||
                   strstr(name, "run_command") || strcmp(name, "bash") == 0))
         shell = 1;
      if ((name && strstr(name, "mcp")) ||
          (ir->tools[i].raw && cJSON_GetObjectItemCaseSensitive(ir->tools[i].raw, "namespace")))
         mcp = 1;
   }
   request_context_note_aimee_session(metrics.tool_calls, metrics.redundant_tool_calls,
                                      metrics.intervention,
                                      shell && mcp ? "both"
                                      : shell      ? "cli"
                                      : mcp        ? "mcp"
                                                   : "none");
}

void aimee_ir_apply_gateway_request_stages(aimee_request_t *request)
{
   aimee_ir_apply_request_stages(request, ir_memory_enabled());
}

void aimee_ir_apply_native_request_stages(aimee_request_t *request)
{
   if (config_present() && config_ingress_preinject_anthropic_enabled())
      aimee_ir_apply_request_stages(request, ir_memory_enabled());
}

char *aimee_ir_build_provider_body(const cJSON *req, const char *driver_name,
                                   const char *agent_model, int max_tokens_override,
                                   int want_stream)
{
   aimee_request_t ir;
   char err[128];
   if (anthropic_frontend_parse(req, &ir, err, sizeof err) != 0)
   {
      aimee_ir_metric_inc(AIMEE_IR_M_PARSE_FAIL, AIMEE_WIRE_ANTHROPIC);
      return NULL;
   }
   /* the served model + cap come from the configured agent, not the client */
   if (agent_model && agent_model[0])
   {
      free(ir.model);
      ir.model = strdup(agent_model);
   }
   if (max_tokens_override > 0)
   {
      ir.max_tokens = max_tokens_override;
      ir.has_max_tokens = 1;
   }
   /* Match the legacy ingress, which defaults an absent temperature to 1.0 and
    * always emits it (build_provider_body is fed jo_num(req, "temperature", 1.0)).
    * Applied at BUILD time, like legacy -- not in the frontend parse, so the IR
    * request itself stays protocol-neutral (an Anthropic-parsed and OpenAI-parsed
    * copy of the same request remain equal). Without it the IR omits temperature
    * when the client sends none, diverging from the legacy provider body. */
   if (!ir.has_temperature)
   {
      ir.temperature = 1.0;
      ir.has_temperature = 1;
   }
   /* The upstream stream flag is the CALLER's decision, not the client's: a caller
    * that streams to the client may still want the upstream reply buffered so it can
    * police + replay it. Inheriting ir.stream from the request is what made the
    * buffered-replay path ask for SSE and then parse it as JSON. */
   ir.stream = want_stream ? 1 : 0;

   aimee_ir_apply_request_stages(&ir, ir_memory_enabled());

   int is_responses = driver_name && strcmp(driver_name, "chatgpt") == 0;
   char *s =
       aimee_backend_render_wire(&ir, is_responses ? AIMEE_WIRE_RESPONSES : AIMEE_WIRE_OPENAI_CHAT);
   aimee_request_free(&ir);
   if (!s)
   {
      aimee_ir_metric_inc(AIMEE_IR_M_BACKEND_BUILD_FAIL, AIMEE_WIRE_ANTHROPIC);
      return NULL;
   }
   if (s)
      aimee_ir_metric_inc(AIMEE_IR_M_IR_PATH, AIMEE_WIRE_ANTHROPIC);
   return s;
}
