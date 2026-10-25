/* test_agent_request_build.c -- golden byte-stability for the single canonical
 * request builder (agent_build_host_request_ir; the legacy per-provider hand-builders were
 * deleted). agent_execute originates provider requests through the IR + per-provider
 * backend; the OUTBOUND bytes must stay stable because Anthropic (and OpenAI) prompt-
 * cache on them. These goldens pin the exact canonical wire per provider so any
 * accidental byte drift in the IR backends fails here.
 *
 * Model-catalog stubs (model_max_output / model_provider_get) keep this a minimal
 * link and make the caps/sampling deterministic; both were consulted identically by
 * every provider path. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "agent_request_build.h"
#include <aimee/module-runtime/pre_llm_hook.h>
#include "model_provider.h"
#include "model_registry.h"
#include "agent_config.h"
#include "cJSON.h"
#include <aimee/ir/session_event.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

int model_max_output(const char *provider, const char *model_id)
{
   (void)provider;
   (void)model_id;
   return 4096;
}
model_provider_t *model_provider_get(const char *name)
{
   (void)name;
   return NULL;
}
/* agent_request_max_tokens() clamps an oversized output cap against the agent's
 * effective context window, falling back to the catalog when no operator
 * override is set. These goldens pin WIRE SHAPE, not capability resolution, so
 * report "no catalog entry" and let the middleware value (unset here) decide. */
const char *agent_catalog_provider(const agent_t *agent)
{
   if (!agent)
      return "";
   return agent->catalog_provider[0] ? agent->catalog_provider : agent->provider;
}
/* Capability stub, driven per-test. The builder now asks the catalog whether a
 * model accepts Anthropic's ADAPTIVE thinking shape, so what this returns is the
 * decision under test: `g_caps_flags` is what a provider (or an operator's
 * capability override) would have reported, and g_caps_known == 0 is the
 * ordinary "nobody has said" case that must fail closed. */
static unsigned g_caps_flags;
static int g_caps_known;

int model_capability_get(const char *provider, const char *model_id, model_capability_t *out)
{
   (void)provider;
   (void)model_id;
   if (!g_caps_known || !out)
      return 0;
   memset(out, 0, sizeof(*out));
   out->flags = g_caps_flags;
   out->context_window = 200000;
   out->max_output = 8192;
   return 1;
}

static void declare_caps(unsigned flags)
{
   g_caps_flags = flags;
   g_caps_known = 1;
}

static void clear_caps(void)
{
   g_caps_flags = 0;
   g_caps_known = 0;
}

static agent_t mk_agent(const char *provider, const char *model)
{
   agent_t a;
   memset(&a, 0, sizeof a);
   snprintf(a.provider, sizeof a.provider, "%s", provider);
   snprintf(a.model, sizeof a.model, "%s", model);
   a.max_tokens = 0; /* unpinned -> derived from the model */
   return a;
}

static cJSON *build_fixture(const agent_t *agent, const char *system, const char *user,
                            int max_tokens, double temperature)
{
   aimee_request_t request = {0};
   assert(!aimee_ir_request_from_text(&request, system, user, max_tokens, temperature));
   cJSON *body = agent_build_host_request_ir(agent, &request);
   aimee_request_free(&request);
   return body;
}

static void golden(const char *provider, const char *model, const char *expect)
{
   agent_t a = mk_agent(provider, model);
   cJSON *req = build_fixture(&a, "You are helpful.", "deploy the release", 100, 0.7);
   assert(req && "builder returned NULL");
   char *got = cJSON_PrintUnformatted(req);
   assert(got);
   if (strcmp(got, expect) != 0)
   {
      printf("  BYTE DRIFT provider=%s\n    expect: %s\n    got   : %s\n", provider, expect, got);
      free(got);
      cJSON_Delete(req);
      exit(1);
   }
   printf("  %s egress byte-stable OK\n", provider);
   free(got);
   cJSON_Delete(req);
}

/* Drive the knob through the REAL config file rather than materialising a legacy_config_record.
 * legacy_config_record is a secret of the config module (check-config-encapsulation), so a test may
 * not name it -- and going through aimee.yaml is the better test anyway: it exercises the
 * extended_thinking parse in config_sections.c, which a struct poke would skip entirely.
 * AIMEE_NO_CACHE forces a re-read per rewrite so on and off can alternate in-process. */
static char g_home[512];

static void init_config_home(void)
{
   char tmpl[256];
   snprintf(tmpl, sizeof tmpl, "%s/aut-xth.XXXXXX", platform_tmpdir());
   const char *d = mkdtemp(tmpl);
   assert(d && "could not create a temp HOME for the config fixture");
   snprintf(g_home, sizeof g_home, "%s", d);
   setenv("HOME", g_home, 1);
   setenv("AIMEE_NO_CACHE", "1", 1);

   char dir[600];
   snprintf(dir, sizeof dir, "%s/.config", g_home);
   assert(mkdir(dir, 0700) == 0);
   snprintf(dir, sizeof dir, "%s/.config/aimee", g_home);
   assert(mkdir(dir, 0700) == 0);
}

static void set_thinking(int enabled, int budget_tokens)
{
   char path[700];
   snprintf(path, sizeof path, "%s/.config/aimee/aimee.yaml", g_home);
   FILE *f = fopen(path, "w");
   assert(f && "could not write the config fixture");
   /* cache_shaping is pinned to its default ON so a thinking assertion never silently
    * also flips the system-block layout the goldens above pin. */
   fprintf(f,
           "cache_shaping:\n"
           "  enabled: true\n"
           "extended_thinking:\n"
           "  enabled: %s\n"
           "  budget_tokens: %d\n",
           enabled ? "true" : "false", budget_tokens);
   fclose(f);
}

static cJSON *build_with_thinking(const char *provider, const char *model, int enabled,
                                  int budget_tokens, int max_tokens)
{
   set_thinking(enabled, budget_tokens);
   agent_t a = mk_agent(provider, model);
   cJSON *req = build_fixture(&a, "You are helpful.", "deploy the release", max_tokens, 0.7);
   assert(req && "builder returned NULL");
   return req;
}

/* The knob asks a model to reason -- but ONLY a model that says it accepts this
 * shape. The config it emits is `{"type":"adaptive"}`: Anthropic removed
 * {"type":"enabled", budget_tokens: N}, which is a 400 on Opus 4.7/4.8/5,
 * Sonnet 5 and Fable 5 and survives only on the 4.6 generation. */
static void test_thinking_adaptive_when_model_supports_it(void)
{
   declare_caps(MODEL_CAP_REASONING | MODEL_CAP_THINKING_ADAPTIVE);
   cJSON *req = build_with_thinking("anthropic", "claude-sonnet-4-6", 1, 2048, 100);
   cJSON *th = cJSON_GetObjectItemCaseSensitive(req, "thinking");
   assert(cJSON_IsObject(th) && "thinking config absent on an aimee-originated turn");
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(th, "type")->valuestring, "adaptive") == 0);
   /* The retired form must not reappear: budget_tokens is what 400s. */
   assert(!cJSON_GetObjectItemCaseSensitive(th, "budget_tokens"));

   /* Sampling is REMOVED, not pinned to temperature=1 as it used to be. Every
    * model reaching this branch is 4.6-or-later, and on 4.7+ any of
    * temperature/top_p/top_k is itself a 400 -- so the old "fix" turned one
    * rejection into two. Removal is valid across the whole range. */
   assert(!cJSON_GetObjectItemCaseSensitive(req, "temperature"));
   assert(!cJSON_GetObjectItemCaseSensitive(req, "top_p"));
   assert(!cJSON_GetObjectItemCaseSensitive(req, "top_k"));

   /* max_tokens is the caller's, untouched: adaptive has no budget to fit under,
    * so the old "raise the cap to clear budget_tokens" arithmetic is gone. */
   cJSON *mt = cJSON_GetObjectItemCaseSensitive(req, "max_tokens");
   assert(cJSON_IsNumber(mt) && mt->valuedouble == 100);
   printf("  adaptive thinking emitted for a capable model OK\n");
   cJSON_Delete(req);
   clear_caps();
}

/* FAIL CLOSED. A model nobody has said accepts adaptive thinking gets no
 * thinking config at all. Sending nothing costs a missed chance to reason;
 * sending the wrong shape costs a 400 the operator sees as an agent failure. */
static void test_thinking_absent_when_capability_unknown(void)
{
   clear_caps();
   cJSON *req = build_with_thinking("anthropic", "some-unknown-model-qqq", 1, 2048, 100);
   assert(!cJSON_GetObjectItemCaseSensitive(req, "thinking"));
   /* And sampling is left exactly as configured, because nothing was stripped. */
   cJSON *temp = cJSON_GetObjectItemCaseSensitive(req, "temperature");
   assert(cJSON_IsNumber(temp) && temp->valuedouble == 0.7);
   printf("  unknown capability emits no thinking OK\n");
   cJSON_Delete(req);
}

/* THE PREDICATE BUG. `is_anth` is the WIRE FORMAT, not the vendor: MiniMax and
 * Moonshot both ship Anthropic-compatible endpoints and are configured with
 * provider="anthropic", catalog_provider="minimax". Enabling this knob used to
 * send Anthropic thinking config to them and to nothing else, since the Claude
 * seat is a provider-CLI agent that never reaches this builder. The capability
 * check excludes them without having to name them. */
static void test_thinking_skips_anthropic_compatible_third_party(void)
{
   /* The vendor is catalogued and reasoning-capable, but has never advertised
    * Anthropic's adaptive thinking -- because it is not an Anthropic model. */
   declare_caps(MODEL_CAP_REASONING | MODEL_CAP_TOOLS);
   agent_t a = mk_agent("anthropic", "MiniMax-M3");
   snprintf(a.catalog_provider, sizeof a.catalog_provider, "minimax");
   set_thinking(1, 2048);
   cJSON *req = build_fixture(&a, "You are helpful.", "deploy the release", 100, 0.7);
   assert(req);
   assert(!cJSON_GetObjectItemCaseSensitive(req, "thinking") &&
          "a third-party model on the anthropic wire must not receive thinking config");
   printf("  anthropic-compatible third party gets no thinking OK\n");
   cJSON_Delete(req);
   clear_caps();
}

/* Anthropic-only. `thinking` is not a field on the OpenAI or Responses wires, so emitting
 * it there would be an unknown-parameter error rather than more reasoning. */
static void test_thinking_is_anthropic_only(void)
{
   const char *others[][2] = {{"openai", "gpt-4o-mini"}, {"chatgpt", "gpt-5.5-codex"}};
   for (int i = 0; i < 2; i++)
   {
      cJSON *req = build_with_thinking(others[i][0], others[i][1], 1, 2048, 100);
      assert(!cJSON_GetObjectItemCaseSensitive(req, "thinking"));
      printf("  %s carries no thinking field OK\n", others[i][0]);
      cJSON_Delete(req);
   }
}

/* Default-off must be byte-identical to the pre-change wire: thinking tokens are billed,
 * so an accidental default flip changes what every aimee turn SPENDS. */
static void test_thinking_disabled_is_unchanged(void)
{
   cJSON *req = build_with_thinking("anthropic", "claude-3-5-sonnet", 0, 2048, 100);
   assert(!cJSON_GetObjectItemCaseSensitive(req, "thinking"));
   cJSON *temp = cJSON_GetObjectItemCaseSensitive(req, "temperature");
   assert(cJSON_IsNumber(temp) && temp->valuedouble == 0.7 && "sampling must be untouched");
   cJSON *mt = cJSON_GetObjectItemCaseSensitive(req, "max_tokens");
   assert(cJSON_IsNumber(mt) && mt->valuedouble == 100);
   printf("  thinking disabled leaves the wire unchanged OK\n");
   cJSON_Delete(req);
}

static void test_typed_sampling_in_plain_requests(void)
{
   const char *providers[] = {"openai", "anthropic", "moonshotai"};
   for (size_t i = 0; i < sizeof providers / sizeof *providers; i++)
   {
      agent_t agent = mk_agent(providers[i], i == 2 ? "k3" : "qwen3.gguf");
      agent.recommended_sampling = 1;
      cJSON *request =
          build_fixture(&agent, "stable<aimee-context>volatile", "inspect", 100, i == 2 ? 0.2 : -1);
      assert(request);
      const cJSON *temperature = cJSON_GetObjectItemCaseSensitive(request, "temperature");
      assert(cJSON_IsNumber(temperature) && temperature->valuedouble == (i == 2 ? 1.0 : 0.6));
      if (i < 2)
      {
         const cJSON *top_p = cJSON_GetObjectItemCaseSensitive(request, "top_p");
         const cJSON *top_k = cJSON_GetObjectItemCaseSensitive(request, "top_k");
         assert(cJSON_IsNumber(top_p) && top_p->valuedouble == 0.95);
         assert(cJSON_IsNumber(top_k) && top_k->valueint == 20);
         const cJSON *min_p = cJSON_GetObjectItemCaseSensitive(request, "min_p");
         assert(i == 0 ? cJSON_IsNumber(min_p) && min_p->valuedouble == 0 : !min_p);
      }
      cJSON_Delete(request);
   }
   puts("typed plain sampling retains recommendations, wire scope and hard constraints OK");
}

static void test_owned_plain_request_preserves_input(void)
{
   aimee_block_t policy = {.type = AIMEE_BLK_TEXT,
                           .text = "Answer using the evidence only.",
                           .context = {.origin = AIMEE_CTX_ORIGIN_USER,
                                       .authority = AIMEE_CTX_AUTH_TASK_INSTRUCTION,
                                       .trust = AIMEE_CTX_TRUST_UNVERIFIED,
                                       .model_visible = 1}};
   aimee_block_t evidence = {.type = AIMEE_BLK_TEXT,
                             .text = "Retrieved source: Orion.",
                             .context = {.origin = AIMEE_CTX_ORIGIN_RETRIEVAL,
                                         .authority = AIMEE_CTX_AUTH_EVIDENCE,
                                         .trust = AIMEE_CTX_TRUST_UNVERIFIED,
                                         .model_visible = 1}};
   aimee_message_t message = {.role = "user", .blocks = &evidence, .n_blocks = 1};
   aimee_request_t input = {.system = &policy,
                            .n_system = 1,
                            .messages = &message,
                            .n_messages = 1,
                            .tool_choice = AIMEE_TOOL_CHOICE_NONE,
                            .max_tokens = 256,
                            .has_max_tokens = 1,
                            .temperature = 0.3,
                            .has_temperature = 1};
   cJSON *snapshot = aimee_ir_request_json(&input);
   assert(snapshot);
   const char *providers[] = {"openai", "anthropic"};
   for (size_t i = 0; i < sizeof providers / sizeof *providers; i++)
   {
      agent_t agent = mk_agent(providers[i], "configured-model");
      cJSON *body = agent_build_plain_request_ir(&agent, &input);
      assert(body);
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(body, "model")), "configured-model"));
      assert(cJSON_GetObjectItem(body, "max_tokens")->valueint == 256);
      assert(cJSON_GetObjectItem(body, "temperature")->valuedouble == 0.3);
      char *encoded = cJSON_PrintUnformatted(body);
      assert(encoded && strstr(encoded, evidence.text) && strstr(encoded, policy.text));
      assert(!cJSON_GetObjectItem(body, "tools"));
      cJSON *after = aimee_ir_request_json(&input);
      assert(after && cJSON_Compare(snapshot, after, 1));
      cJSON_Delete(after);
      cJSON_Delete(body);
      free(encoded);
      aimee_request_t host = input;
      host.tool_choice = AIMEE_TOOL_CHOICE_UNSET;
      body = agent_build_host_request_ir(&agent, &host);
      assert(body);
      encoded = cJSON_PrintUnformatted(body);
      assert(encoded && strstr(encoded, evidence.text) && strstr(encoded, policy.text));
      after = aimee_ir_request_json(&input);
      assert(after && cJSON_Compare(snapshot, after, 1));
      cJSON_Delete(after);
      cJSON_Delete(body);
      free(encoded);
   }
   agent_t agent = mk_agent("openai", "configured-model");
   input.stream = 1;
   assert(!agent_build_plain_request_ir(&agent, &input));
   input.stream = 0;
   input.model = "unrelated-pinned-model";
   assert(!agent_build_plain_request_ir(&agent, &input));
   cJSON_Delete(snapshot);
   puts("owned plain input preserves evidence provenance, exact controls and configured target OK");
}

static int fixture_context_hook(const char *view, char *out, size_t size, void *user)
{
   (void)user;
   assert(strstr(view, "Original task") && strstr(view, "Evidence 雪"));
   assert(!strstr(view, "private host policy"));
   snprintf(out, size, "Ephemeral hook evidence");
   return 0;
}

static void test_plain_hook_context_is_separate_ir_evidence(void)
{
   aimee_block_t policy = {.type = AIMEE_BLK_TEXT, .text = "private host policy"};
   aimee_block_t blocks[] = {{.type = AIMEE_BLK_TEXT,
                              .text = "Original task",
                              .message_group = 2,
                              .context = {.origin = AIMEE_CTX_ORIGIN_USER,
                                          .authority = AIMEE_CTX_AUTH_TASK_INSTRUCTION,
                                          .trust = AIMEE_CTX_TRUST_UNVERIFIED,
                                          .model_visible = 1}},
                             {.type = AIMEE_BLK_TEXT,
                              .text = "Evidence 雪",
                              .message_group = 3,
                              .context = {.origin = AIMEE_CTX_ORIGIN_RETRIEVAL,
                                          .authority = AIMEE_CTX_AUTH_EVIDENCE,
                                          .trust = AIMEE_CTX_TRUST_UNVERIFIED,
                                          .model_visible = 1}}};
   aimee_message_t message = {.role = "user", .blocks = blocks, .n_blocks = 2};
   aimee_request_t input = {.system = &policy,
                            .n_system = 1,
                            .messages = &message,
                            .n_messages = 1,
                            .has_temperature = 1,
                            .temperature = 0.0,
                            .has_max_tokens = 1,
                            .max_tokens = 128},
                   prepared = {0};
   cJSON *snapshot = aimee_ir_request_json(&input);
   assert(snapshot);
   plugin_chook_reset();
   assert(!plugin_chook_register_pre_llm(fixture_context_hook, NULL));
   assert(agent_prepare_plain_request_hooks(&input, &prepared) == 1);
   assert(prepared.n_messages == 1 && prepared.messages[0].n_blocks == 3);
   for (int i = 0; i < 2; i++)
   {
      cJSON *old = aimee_ir_block_json(&blocks[i]);
      cJSON *copy = aimee_ir_block_json(&prepared.messages[0].blocks[i]);
      assert(old && copy && cJSON_Compare(old, copy, 1));
      cJSON_Delete(old);
      cJSON_Delete(copy);
   }
   const aimee_block_t *context = &prepared.messages[0].blocks[2];
   assert(!strcmp(context->text, "Ephemeral hook evidence"));
   assert(context->context.origin == AIMEE_CTX_ORIGIN_MEMORY &&
          context->context.authority == AIMEE_CTX_AUTH_EVIDENCE &&
          context->context.trust == AIMEE_CTX_TRUST_UNVERIFIED);
   agent_t agent = mk_agent("openai", "configured-model");
   cJSON *native = agent_build_host_request_ir(&agent, &prepared);
   assert(native);
   char *rendered = cJSON_PrintUnformatted(native);
   assert(rendered && strstr(rendered, blocks[0].text) && strstr(rendered, blocks[1].text) &&
          strstr(rendered, context->text));
   free(rendered);
   cJSON_Delete(native);
   cJSON *after = aimee_ir_request_json(&input);
   assert(after && cJSON_Compare(snapshot, after, 1));
   cJSON_Delete(after);
   cJSON_Delete(snapshot);
   aimee_request_free(&prepared);
   plugin_chook_reset();
   assert(!agent_prepare_plain_request_hooks(&input, &prepared) && !prepared.n_messages);
   puts("plain hooks preserve original IR spans and append separate unverified evidence OK");
}

static int fixture_history_hook(const char *view, char *out, size_t size, void *user)
{
   (void)user;
   assert(strstr(view, "Repair the owned draft.") && strstr(view, "Retrieved repair evidence."));
   assert(!strstr(view, "PRIVATE"));
   snprintf(out, size, "History hook context.");
   return 0;
}

static void test_canonical_history_egress_projection(void)
{
   aimee_block_t policies[] = {{.type = AIMEE_BLK_TEXT, .text = "Keep host policy A."},
                               {.type = AIMEE_BLK_TEXT, .text = "Keep appended host policy B."},
                               {.type = AIMEE_BLK_TEXT,
                                .text = "PRIVATE SYSTEM SECRET",
                                .context = {.origin = AIMEE_CTX_ORIGIN_PLATFORM,
                                            .authority = AIMEE_CTX_AUTH_POLICY,
                                            .trust = AIMEE_CTX_TRUST_VERIFIED,
                                            .sensitivity = AIMEE_CTX_SENS_RESTRICTED,
                                            .model_visible = 0}}};
   aimee_block_t task = {.type = AIMEE_BLK_TEXT, .text = "Original repair task."};
   aimee_block_t prior[] = {{.type = AIMEE_BLK_TEXT, .text = "Prior draft remains evidence."},
                            {.type = AIMEE_BLK_THINKING,
                             .text = "PRIVATE REASONING PAYLOAD",
                             .thinking_signature = "PRIVATE THINKING SIGNATURE"}};
   aimee_ir_block_set_context(&prior[1], AIMEE_CTX_ORIGIN_MODEL, AIMEE_CTX_AUTH_DIAGNOSTIC,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 0,
                              "model-output", "cron-repair", 3);
   aimee_block_t repair[] = {{.type = AIMEE_BLK_TEXT, .text = "Repair the owned draft."},
                             {.type = AIMEE_BLK_TEXT, .text = "Retrieved repair evidence."}};
   aimee_ir_block_set_context(&repair[1], AIMEE_CTX_ORIGIN_RETRIEVAL, AIMEE_CTX_AUTH_EVIDENCE,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1,
                              "repair-evidence", "source", 4);
   aimee_message_t history[] = {{.role = "user",
                                 .blocks = &task,
                                 .n_blocks = 1,
                                 .content_scalar = 1,
                                 .compaction_boundary = 1},
                                {.role = "assistant", .blocks = prior, .n_blocks = 2},
                                {.role = "user", .blocks = repair, .n_blocks = 2}};
   aimee_request_t request = {.system = policies,
                              .n_system = 3,
                              .messages = history,
                              .n_messages = 3,
                              .has_max_tokens = 1,
                              .max_tokens = 128,
                              .has_temperature = 1,
                              .temperature = 0.0};
   request.raw = cJSON_Parse("{\"messages\":[{\"content\":\"PRIVATE REASONING PAYLOAD\"}]}");
   cJSON *snapshot = aimee_ir_request_json(&request),
         *raw_snapshot = cJSON_Duplicate(request.raw, 1);
   assert(snapshot && raw_snapshot && agent_host_request_supported(&request, 0));
   aimee_request_t projected = {0};
   assert(!agent_request_project_model(&request, &projected));
   assert(projected.n_messages == 3 && projected.n_system == 2 &&
          projected.messages[1].n_blocks == 1 && projected.messages[0].content_scalar &&
          projected.messages[0].compaction_boundary && !projected.raw);
   assert(!strcmp(projected.messages[1].blocks[0].text, prior[0].text));
   assert(projected.messages[2].blocks[1].context.origin == AIMEE_CTX_ORIGIN_RETRIEVAL &&
          projected.messages[2].blocks[1].context.authority == AIMEE_CTX_AUTH_EVIDENCE &&
          projected.messages[2].blocks[1].context.revision_epoch == 4);
   aimee_request_free(&projected);
   const char *providers[] = {"openai", "anthropic", "chatgpt"};
   for (size_t i = 0; i < sizeof providers / sizeof providers[0]; i++)
   {
      agent_t agent = mk_agent(providers[i], "configured-model");
      cJSON *body = agent_build_host_request_ir(&agent, &request);
      assert(body);
      char *encoded = cJSON_PrintUnformatted(body);
      assert(encoded && strstr(encoded, policies[0].text) && strstr(encoded, policies[1].text) &&
             strstr(encoded, task.text) && strstr(encoded, prior[0].text) &&
             strstr(encoded, repair[0].text) && strstr(encoded, repair[1].text));
      assert(!strstr(encoded, "PRIVATE REASONING PAYLOAD") &&
             !strstr(encoded, "PRIVATE THINKING SIGNATURE") &&
             !strstr(encoded, "PRIVATE SYSTEM SECRET"));
      cJSON *messages = cJSON_GetObjectItemCaseSensitive(
          body, !strcmp(providers[i], "chatgpt") ? "input" : "messages");
      assert(cJSON_IsArray(messages));
      if (strcmp(providers[i], "chatgpt"))
      {
         int offset = !strcmp(providers[i], "openai") ? 2 : 0;
         assert(cJSON_GetArraySize(messages) == 3 + offset);
         assert(!strcmp(cJSON_GetStringValue(
                            cJSON_GetObjectItem(cJSON_GetArrayItem(messages, offset), "role")),
                        "user"));
         assert(!strcmp(cJSON_GetStringValue(
                            cJSON_GetObjectItem(cJSON_GetArrayItem(messages, offset + 1), "role")),
                        "assistant"));
      }
      free(encoded);
      cJSON_Delete(body);
      cJSON *after = aimee_ir_request_json(&request);
      assert(after && cJSON_Compare(snapshot, after, 1) &&
             cJSON_Compare(request.raw, raw_snapshot, 1));
      cJSON_Delete(after);
   }
   plugin_chook_reset();
   assert(!plugin_chook_register_pre_llm(fixture_history_hook, NULL));
   aimee_request_t prepared = {0};
   assert(agent_prepare_plain_request_hooks(&request, &prepared) == 1);
   assert(prepared.n_messages == 3 && prepared.n_system == 3 &&
          prepared.messages[1].n_blocks == 2 && prepared.messages[2].n_blocks == 3);
   assert(!strcmp(prepared.messages[2].blocks[2].text, "History hook context."));
   assert(prepared.messages[2].blocks[2].context.authority == AIMEE_CTX_AUTH_EVIDENCE);
   aimee_request_free(&prepared);
   plugin_chook_reset();
   cJSON *after_hook = aimee_ir_request_json(&request);
   assert(after_hook && cJSON_Compare(snapshot, after_hook, 1));
   cJSON_Delete(after_hook);
   /* Cron carries completed assistant blocks as user evidence; hidden reasoning
    * stays owned internally while each actual provider sees the visible task. */
   history[1].role = "user";
   aimee_request_t cron = request;
   cron.messages = &history[1];
   cron.n_messages = 1;
   cJSON *cron_snapshot = aimee_ir_request_json(&cron);
   for (size_t i = 0; i < sizeof providers / sizeof providers[0]; i++)
   {
      agent_t agent = mk_agent(providers[i], "configured-model");
      cJSON *body = agent_build_host_request_ir(&agent, &cron);
      assert(body);
      char *encoded = cJSON_PrintUnformatted(body);
      assert(encoded && strstr(encoded, prior[0].text) && !strstr(encoded, "PRIVATE"));
      cJSON *after = aimee_ir_request_json(&cron);
      assert(after && cJSON_Compare(cron_snapshot, after, 1));
      cJSON_Delete(after);
      cJSON_Delete(body);
      free(encoded);
   }
   cJSON_Delete(cron_snapshot);
   cJSON_Delete(snapshot);
   cJSON_Delete(raw_snapshot);
   cJSON_Delete(request.raw);
   puts("repair histories and all policy/evidence blocks reach real provider builders without "
        "hidden reasoning or source mutation OK");
}

typedef struct
{
   int events, controls, empty, reject;
} projection_fixture_t;

static int fixture_projection_sink(const aimee_session_event_t *event, void *context)
{
   projection_fixture_t *fixture = context;
   assert(event->kind == AIMEE_SESSION_PROJECTION &&
          !strcmp(event->projection_boundary, "request"));
   assert(!event->projection_reasoning && !event->projection_opaque &&
          !event->projection_cache_controls &&
          (event->projection_request_controls > 0 || event->projection_empty_messages > 0));
   cJSON *encoded = aimee_ir_session_event_json(event);
   assert(encoded);
   cJSON_Delete(encoded);
   fixture->events++;
   fixture->controls = event->projection_request_controls;
   fixture->empty = event->projection_empty_messages;
   return fixture->reject;
}

static void test_host_control_projection_capture(void)
{
   agent_t agent = mk_agent("chatgpt", "gpt-5.5-codex");
   snprintf(agent.name, sizeof agent.name, "codex-fixture");
   aimee_request_t input = {0};
   assert(!aimee_ir_request_from_text(&input, NULL, "Keep my source controls.", 128, 0.3));
   input.has_top_p = input.has_top_k = 1;
   input.top_p = 0.8;
   input.top_k = 17;
   input.thinking = cJSON_Parse("{\"type\":\"disabled\"}");
   cJSON *snapshot = aimee_ir_request_json(&input);
   projection_fixture_t fixture = {0};
   aimee_ir_session_set_sink(fixture_projection_sink, &fixture);
   cJSON *body = agent_build_host_request_ir(&agent, &input);
   assert(body && fixture.events == 1 && fixture.controls == 5);
   assert(!cJSON_HasObjectItem(body, "max_output_tokens") &&
          !cJSON_HasObjectItem(body, "temperature"));
   cJSON_Delete(body);
   fixture.reject = 1;
   assert(!agent_build_host_request_ir(&agent, &input) && aimee_ir_session_capture_failed());
   aimee_ir_session_set_sink(NULL, NULL);
   aimee_ir_session_begin_task();
   cJSON *after = aimee_ir_request_json(&input);
   assert(after && cJSON_Compare(snapshot, after, 1));
   cJSON_Delete(after);
   cJSON_Delete(snapshot);
   aimee_request_free(&input);
   assert(!aimee_ir_request_from_text(&input, NULL, "No explicit controls to omit.", 0, -1));
   fixture = (projection_fixture_t){0};
   aimee_ir_session_set_sink(fixture_projection_sink, &fixture);
   body = agent_build_host_request_ir(&agent, &input);
   assert(body && !fixture.events);
   cJSON_Delete(body);
   aimee_ir_session_set_sink(NULL, NULL);
   aimee_ir_session_begin_task();
   aimee_request_free(&input);
   aimee_block_t words[] = {{.type = AIMEE_BLK_TEXT, .text = "old task"},
                            {.type = AIMEE_BLK_TEXT, .text = "continue task"}};
   aimee_message_t history[] = {{.role = "user", .blocks = &words[0], .n_blocks = 1},
                                {.role = "assistant"},
                                {.role = "user", .blocks = &words[1], .n_blocks = 1}};
   aimee_request_t historical = {.messages = history, .n_messages = 3};
   cJSON *historical_before = aimee_ir_request_json(&historical);
   agent = mk_agent("anthropic", "configured-claude");
   for (int rejected = 0; rejected < 2; rejected++)
   {
      fixture = (projection_fixture_t){.reject = rejected};
      aimee_ir_session_set_sink(fixture_projection_sink, &fixture);
      body = agent_build_host_request_ir(&agent, &historical);
      assert(fixture.events == 1 && fixture.empty == 1 && !fixture.controls);
      assert(rejected ? !body && aimee_ir_session_capture_failed() : body != NULL);
      cJSON_Delete(body);
      aimee_ir_session_set_sink(NULL, NULL);
      aimee_ir_session_begin_task();
      cJSON *after = aimee_ir_request_json(&historical);
      assert(after && cJSON_Compare(historical_before, after, 1));
      cJSON_Delete(after);
   }
   cJSON_Delete(historical_before);
   puts("actual omitted host controls report exact canonical projection counts and require active "
        "capture OK");
}

int main(void)
{
   init_config_home();
   set_thinking(0, 2048);
   test_owned_plain_request_preserves_input();
   test_plain_hook_context_is_separate_ir_evidence();
   test_canonical_history_egress_projection();
   test_host_control_projection_capture();
   printf("agent_request_build golden:\n");

   /* Anthropic Messages wire: system + user as typed text blocks, max_tokens, then
    * temperature (from model_sampling). This is the HARD byte-identity surface. The
    * system block carries the uniform aimee cache_control policy (mark_cache_prefix):
    * the canonical Anthropic egress caches the stable system prefix on every source.
    * With cache_shaping_enabled default-ON, the builder re-splits + re-marks the system
    * at the <aimee-context> boundary (agent_request_build.c), which re-adds `system`
    * after `messages` — a deterministic, byte-stable layout; key order is not
    * semantically significant to Anthropic and the cache_control content is unchanged. */
   golden("anthropic", "claude-3-5-sonnet",
          "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":100,"
          "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\","
          "\"text\":\"deploy the release\"}]}],"
          "\"system\":[{\"type\":\"text\",\"text\":\"You are helpful.\","
          "\"cache_control\":{\"type\":\"ephemeral\"}}],\"temperature\":0.7}");

   /* OpenAI Chat Completions wire. */
   golden("openai", "gpt-4o-mini",
          "{\"model\":\"gpt-4o-mini\",\"max_tokens\":100,"
          "\"messages\":[{\"role\":\"system\",\"content\":\"You are helpful.\"},"
          "{\"role\":\"user\",\"content\":\"deploy the release\"}],\"temperature\":0.7}");

   /* Responses (codex) wire: store=false, stream=true, no max_tokens; canonical input
    * item shape. */
   golden("chatgpt", "gpt-5.5-codex",
          "{\"model\":\"gpt-5.5-codex\",\"store\":false,\"stream\":true,"
          "\"include\":[\"reasoning.encrypted_content\"],"
          "\"instructions\":\"You are helpful.\","
          "\"input\":[{\"type\":\"message\",\"role\":\"user\","
          "\"content\":[{\"type\":\"input_text\",\"text\":\"deploy the release\"}]}]}");

   printf("extended thinking:\n");
   test_thinking_adaptive_when_model_supports_it();
   test_thinking_absent_when_capability_unknown();
   test_thinking_skips_anthropic_compatible_third_party();
   test_thinking_is_anthropic_only();
   test_thinking_disabled_is_unchanged();

   test_typed_sampling_in_plain_requests();
   printf("all agent_request_build goldens passed\n");
   return 0;
}
