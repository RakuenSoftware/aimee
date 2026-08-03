/* config_kb_curator.c: parser for the `kb.curator` config section.
 * Split out of config.c to keep that file under the line-count gate.
 *
 * kb:
 *   curator:
 *     extract_docs:
 *       enabled: false
 *     extract_code:
 *       enabled: false
 *     extract_command: ""
 *     extract_max_tokens: 4096
 *     max_attempts: 3
 */

#include "aimee.h"
#include "config.h"
#include "json_fluent.h"
#include "cJSON.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Preset -> stage membership. LITE is the cheap, high-value core: pull facts
 * (extract_docs/extract_code), index them (narrative/claims), resolve entities.
 * FULL adds the heavier synthesis/graph/contradiction/promotion stages. OFF
 * disables all curation. Unknown tier -> FULL (fail-open, matches the historical
 * all-on default). */
static int kb_curator_tier_stage_on(const char *tier, int lite_member)
{
   if (!tier || !tier[0] || strcmp(tier, "full") == 0)
      return 1;
   if (strcmp(tier, "off") == 0)
      return 0;
   if (strcmp(tier, "lite") == 0)
      return lite_member;
   return 1; /* unknown -> full */
}

/* Drive the 12 kb_curator_*_enabled stage gates from the preset tier. Reader-free:
 * every consumer keeps reading cfg->kb_curator_<stage>_enabled; only its default
 * source changes. An explicit per-stage gate in the config still overrides, since
 * config_parse_kb_curator applies the tier first and the per-stage keys after. */
static void kb_curator_apply_tier(config_t *cfg, const char *tier)
{
   cfg->kb_curator_extract_docs_enabled = kb_curator_tier_stage_on(tier, 1);
   cfg->kb_curator_extract_code_enabled = kb_curator_tier_stage_on(tier, 1);
   cfg->kb_curator_resolve_entities_enabled = kb_curator_tier_stage_on(tier, 1);
   cfg->kb_curator_index_narrative_enabled = kb_curator_tier_stage_on(tier, 1);
   cfg->kb_curator_index_claims_enabled = kb_curator_tier_stage_on(tier, 1);
   cfg->kb_curator_detect_contradictions_enabled = kb_curator_tier_stage_on(tier, 0);
   cfg->kb_curator_index_code_unit_enabled = kb_curator_tier_stage_on(tier, 0);
   cfg->kb_curator_link_artifacts_enabled = kb_curator_tier_stage_on(tier, 0);
   cfg->kb_curator_projection_graph_enabled = kb_curator_tier_stage_on(tier, 0);
   cfg->kb_curator_synthesize_enabled = kb_curator_tier_stage_on(tier, 0);
   cfg->kb_curator_promote_entity_enabled = kb_curator_tier_stage_on(tier, 0);
   cfg->kb_curator_cross_repo_graph_enabled = kb_curator_tier_stage_on(tier, 0);
}

void config_kb_curator_defaults(config_t *cfg)
{
   /* The deep-curator (larger LLM) pipeline is default-ON: it drains the backlog
    * continuously in the background (no artificial rate cap) and refines what the
    * 0.6B embedder already indexed. Every stage degrades to a no-op when its
    * prerequisite (a configured curator/judge/synthesize command, or upstream
    * curator rows) is absent, so default-on is safe on a bare deploy. The 12 stage
    * gates are driven by the kb_curator_tier preset (default "full" = every stage,
    * i.e. the historical all-on default); see kb_curator_apply_tier. */
   snprintf(cfg->kb_curator_tier, sizeof(cfg->kb_curator_tier), "full");
   kb_curator_apply_tier(cfg, cfg->kb_curator_tier);
   cfg->kb_curator_extract_prompt_version[0] = '\0';
   cfg->kb_curator_embed_model_version[0] = '\0';
   cfg->kb_curator_invalidation_notify_socket[0] = '\0';
   /* kb.typed_facts.* autonomous reconciliation (§7.2): auto-promote recurrent
    * provisional relations by default so novel-tail facts become durable. */
   cfg->kb_typed_facts_auto_promote_enabled = 1;
   cfg->kb_typed_facts_promote_threshold = 3;
   cfg->kb_curator_extract_code_workers = 1;
   cfg->kb_curator_extract_docs_workers = 1;
   cfg->kb_curator_promote_min_sources = 3;
   cfg->kb_curator_extract_command[0] = '\0';
   cfg->kb_curator_stage_order[0] = '\0';
   cfg->kb_curator_user_presets[0] = '\0';
   cfg->kb_curator_custom_stages[0] = '\0';
   cfg->kb_curator_judge_command[0] = '\0';
   cfg->kb_curator_synthesize_command[0] = '\0';
   cfg->kb_curator_synthesize_k = 8;
   /* SIZED FOR A MODEL THAT THINKS BEFORE IT ANSWERS, which 512 was not.
    *
    * 512 came from a 2-4 tokens/s bundled synth with no reasoning pass, chosen to
    * stay inside the provider deadline of the day, then five minutes -- see
    * KB_CURATOR_PROVIDER_TIMEOUT_MS, which E4B's measured rate has since moved. The bundled synth
    * is now gemma-4 E2B/E4B, which reasons before emitting content: on the shipped extract prompt
    * it spent ~290 tokens reasoning and then had the JSON envelope cut off mid-object at exactly
    * 512. cJSON_Parse failed, the job was marked "sidecar returned non-JSON", and it retried into
    * permanent failure -- every extract_doc job, every time, on the default configuration.
    *
    * 4096 IS THE SAME NUMBER bench/tier-a ALREADY ARRIVED AT, and arriving at it a
    * second time by a shorter route is the whole argument for the comment. Its
    * Defect 16: run_b.py capped completions at 1024, "with thinking left on, models
    * spend 400-1000 tokens reasoning before a short answer", gemma-4-12B truncated on
    * one topic, scored zero, and the truncation was reported as a model finding. The
    * fix there was a 4096 cap and a scorer that refuses to score a truncated row.
    *
    * The measurement here, on the deployed E2B sidecar with thinking on: a
    * one-sentence document needs 767 tokens, a single realistic paragraph needs 1087.
    * A 1024 cap is therefore below what an ordinary document costs -- it is inside the
    * band the log says reasoning ALONE occupies.
    *
    * THE CAP IS A CEILING, NOT AN EXPECTED COST, and the two ways of exceeding it are
    * not equally bad. Truncating at the cap yields invalid JSON, which is permanent
    * after max_attempts. Exceeding the 300s provider deadline is a transport error,
    * which kb_curator_error_is_provider_unavailable treats as retryable with backoff.
    * A generous cap degrades gracefully; a tight one fails for good. At the measured
    * ~10 tokens/s a typical extraction is 118s, and only a document that reasoned past
    * ~3000 tokens would reach the deadline at all.
    *
    * Do NOT "fix" the cost by turning synthesis_thinking off. That was measured too:
    * bench/tier-a records disable_thinking costing E4B 0.09 F1 against a 0.582
    * baseline, and its Defect 24 is a whole sweep that ran reasoning models with
    * reasoning suppressed and wrongly concluded they were worse. */
   cfg->kb_curator_extract_max_tokens = 4096;
   cfg->kb_curator_max_attempts = 3;
   cfg->kb_curator_provider_base_url[0] = '\0';
   cfg->kb_curator_provider_model[0] = '\0';
   cfg->kb_curator_provider_api_key[0] = '\0';
   cfg->kb_evidence_embed_enabled = 1;
   cfg->kb_evidence_embed_batch = 32;
   /* Cross-repo dependency graph — initial calibration (re-tuned vs S8 fixtures).
    * The _enabled gate is driven by kb_curator_tier (applied above). */
   cfg->kb_curator_cross_repo_distinctiveness_v = 1;
   cfg->kb_curator_cross_repo_k = 5;
   cfg->kb_curator_cross_repo_m = 8;
   cfg->kb_curator_cross_repo_p_pct = 25;
   cfg->kb_curator_cross_repo_len_min = 4;
   cfg->kb_curator_cross_repo_caller_collision_c = 5;
   cfg->kb_curator_cross_repo_max_candidates = 50000;
   cfg->kb_curator_cross_repo_query_timeout_ms = 5000;
   cfg->kb_curator_cross_repo_review_queue_max = 5000;
}

/* Parse a curator provider sub-object {base_url, model, api_key} under `curator`
 * into the given fields. Missing keys leave the field untouched (its default). */
static void parse_curator_provider(const cJSON *curator, const char *key, char *base_url,
                                   size_t bsz, char *model, size_t msz, char *api_key, size_t ksz)
{
   const cJSON *p = cJSON_GetObjectItemCaseSensitive(curator, key);
   if (!p)
      return;
   if (!cJSON_IsObject(p))
   {
      config_issue("\"kb.curator.%s\" expected object, got %s", key, jo_type_name(p));
      return;
   }
   const cJSON *b = cJSON_GetObjectItemCaseSensitive(p, "base_url");
   if (b)
   {
      if (!cJSON_IsString(b) || !b->valuestring)
         config_issue("\"kb.curator.%s.base_url\" expected string, got %s", key, jo_type_name(b));
      else
         snprintf(base_url, bsz, "%s", b->valuestring);
   }
   const cJSON *m = cJSON_GetObjectItemCaseSensitive(p, "model");
   if (m)
   {
      if (!cJSON_IsString(m) || !m->valuestring)
         config_issue("\"kb.curator.%s.model\" expected string, got %s", key, jo_type_name(m));
      else
         snprintf(model, msz, "%s", m->valuestring);
   }
   const cJSON *k = cJSON_GetObjectItemCaseSensitive(p, "api_key");
   if (k)
   {
      if (!cJSON_IsString(k) || !k->valuestring)
         config_issue("\"kb.curator.%s.api_key\" expected string, got %s", key, jo_type_name(k));
      else
         snprintf(api_key, ksz, "%s", k->valuestring);
   }

   /* A provider is only usable with a base_url + model; warn on a partial object
    * (e.g. a key/model with no endpoint) so it doesn't silently resolve to idle. */
   if (base_url[0] && !model[0])
      config_issue("\"kb.curator.%s\" has base_url but no model (provider unusable)", key);
   if (!base_url[0] && (model[0] || api_key[0]))
      config_issue("\"kb.curator.%s\" set without base_url (tier stays idle)", key);
}

int config_parse_kb_curator(config_t *cfg, const cJSON *root)
{
   const cJSON *kb = cJSON_GetObjectItemCaseSensitive(root, "kb");
   if (!kb)
      return 0;
   if (!cJSON_IsObject(kb))
      return config_issue("\"kb\" expected object, got %s", jo_type_name(kb));

   /* kb.typed_facts.* — KB-owned autonomous reconciliation knobs (§7.2/§8). Parsed
    * before the kb.curator early-return so it applies even without a curator block. */
   const cJSON *tf = cJSON_GetObjectItemCaseSensitive(kb, "typed_facts");
   if (tf)
   {
      if (!cJSON_IsObject(tf))
         return config_issue("\"kb.typed_facts\" expected object, got %s", jo_type_name(tf));
      /* KB-owned master gate: kb.typed_facts.enabled aliases typed_facts_enabled so
       * the whole layer is enabled/disabled from the KB config surface (and the
       * console), keeping aimee-server out of the loop. */
      const cJSON *en = cJSON_GetObjectItemCaseSensitive(tf, "enabled");
      if (en && cJSON_IsBool(en))
         cfg->typed_facts_enabled = cJSON_IsTrue(en) ? 1 : 0;
      const cJSON *ap = cJSON_GetObjectItemCaseSensitive(tf, "auto_promote");
      if (ap && cJSON_IsBool(ap))
         cfg->kb_typed_facts_auto_promote_enabled = cJSON_IsTrue(ap) ? 1 : 0;
      const cJSON *pt = cJSON_GetObjectItemCaseSensitive(tf, "promote_threshold");
      if (pt && cJSON_IsNumber(pt) && pt->valueint > 0)
         cfg->kb_typed_facts_promote_threshold = pt->valueint;
   }

   const cJSON *curator = cJSON_GetObjectItemCaseSensitive(kb, "curator");
   if (!curator)
      return 0;
   if (!cJSON_IsObject(curator))
      return config_issue("\"kb.curator\" expected object, got %s", jo_type_name(curator));

   /* Preset: kb.curator.tier drives all 12 stage gates. Applied FIRST so the
    * per-stage `enabled` keys below still override it (back-compat with configs
    * that pin individual stages). */
   const cJSON *tier = cJSON_GetObjectItemCaseSensitive(curator, "tier");
   if (tier && cJSON_IsString(tier) && tier->valuestring[0])
   {
      snprintf(cfg->kb_curator_tier, sizeof(cfg->kb_curator_tier), "%s", tier->valuestring);
      kb_curator_apply_tier(cfg, cfg->kb_curator_tier);
   }

   const cJSON *extract_docs = cJSON_GetObjectItemCaseSensitive(curator, "extract_docs");
   if (extract_docs)
   {
      if (!cJSON_IsObject(extract_docs))
         return config_issue("\"kb.curator.extract_docs\" expected object, got %s",
                             jo_type_name(extract_docs));
      const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(extract_docs, "enabled");
      if (enabled && cJSON_IsBool(enabled))
         cfg->kb_curator_extract_docs_enabled = cJSON_IsTrue(enabled) ? 1 : 0;
      /* workers: dedicated extract_doc drain threads (see config.h). Clamped
       * to [1,8]; non-numeric / out-of-range values keep the default. */
      const cJSON *workers = cJSON_GetObjectItemCaseSensitive(extract_docs, "workers");
      if (workers && cJSON_IsNumber(workers) && workers->valueint >= 1 && workers->valueint <= 8)
         cfg->kb_curator_extract_docs_workers = workers->valueint;
   }

   const cJSON *extract_code = cJSON_GetObjectItemCaseSensitive(curator, "extract_code");
   if (extract_code)
   {
      if (!cJSON_IsObject(extract_code))
         return config_issue("\"kb.curator.extract_code\" expected object, got %s",
                             jo_type_name(extract_code));
      const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(extract_code, "enabled");
      if (enabled && cJSON_IsBool(enabled))
         cfg->kb_curator_extract_code_enabled = cJSON_IsTrue(enabled) ? 1 : 0;
      /* workers: dedicated extract_code drain threads (see config.h). Clamped
       * to [1,8]; non-numeric / out-of-range values keep the default. */
      const cJSON *workers = cJSON_GetObjectItemCaseSensitive(extract_code, "workers");
      if (workers && cJSON_IsNumber(workers) && workers->valueint >= 1 && workers->valueint <= 8)
         cfg->kb_curator_extract_code_workers = workers->valueint;
   }

   const cJSON *resolve_entities = cJSON_GetObjectItemCaseSensitive(curator, "resolve_entities");
   if (resolve_entities)
   {
      if (!cJSON_IsObject(resolve_entities))
         return config_issue("\"kb.curator.resolve_entities\" expected object, got %s",
                             jo_type_name(resolve_entities));
      const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(resolve_entities, "enabled");
      if (enabled && cJSON_IsBool(enabled))
         cfg->kb_curator_resolve_entities_enabled = cJSON_IsTrue(enabled) ? 1 : 0;
   }

   const cJSON *index_narrative = cJSON_GetObjectItemCaseSensitive(curator, "index_narrative");
   if (index_narrative)
   {
      if (!cJSON_IsObject(index_narrative))
         return config_issue("\"kb.curator.index_narrative\" expected object, got %s",
                             jo_type_name(index_narrative));
      const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(index_narrative, "enabled");
      if (enabled && cJSON_IsBool(enabled))
         cfg->kb_curator_index_narrative_enabled = cJSON_IsTrue(enabled) ? 1 : 0;
   }

   const cJSON *index_claims = cJSON_GetObjectItemCaseSensitive(curator, "index_claims");
   if (index_claims)
   {
      if (!cJSON_IsObject(index_claims))
         return config_issue("\"kb.curator.index_claims\" expected object, got %s",
                             jo_type_name(index_claims));
      const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(index_claims, "enabled");
      if (enabled && cJSON_IsBool(enabled))
         cfg->kb_curator_index_claims_enabled = cJSON_IsTrue(enabled) ? 1 : 0;
   }

   const cJSON *detect_contra = cJSON_GetObjectItemCaseSensitive(curator, "detect_contradictions");
   if (detect_contra)
   {
      if (!cJSON_IsObject(detect_contra))
         return config_issue("\"kb.curator.detect_contradictions\" expected object, got %s",
                             jo_type_name(detect_contra));
      const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(detect_contra, "enabled");
      if (enabled && cJSON_IsBool(enabled))
         cfg->kb_curator_detect_contradictions_enabled = cJSON_IsTrue(enabled) ? 1 : 0;
   }

   const cJSON *index_code_unit = cJSON_GetObjectItemCaseSensitive(curator, "index_code_unit");
   if (index_code_unit)
   {
      if (!cJSON_IsObject(index_code_unit))
         return config_issue("\"kb.curator.index_code_unit\" expected object, got %s",
                             jo_type_name(index_code_unit));
      const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(index_code_unit, "enabled");
      if (enabled && cJSON_IsBool(enabled))
         cfg->kb_curator_index_code_unit_enabled = cJSON_IsTrue(enabled) ? 1 : 0;
   }

   const cJSON *link_artifacts = cJSON_GetObjectItemCaseSensitive(curator, "link_artifacts");
   if (link_artifacts)
   {
      if (!cJSON_IsObject(link_artifacts))
         return config_issue("\"kb.curator.link_artifacts\" expected object, got %s",
                             jo_type_name(link_artifacts));
      const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(link_artifacts, "enabled");
      if (enabled && cJSON_IsBool(enabled))
         cfg->kb_curator_link_artifacts_enabled = cJSON_IsTrue(enabled) ? 1 : 0;
   }

   const cJSON *projection_graph = cJSON_GetObjectItemCaseSensitive(curator, "projection_graph");
   if (projection_graph)
   {
      if (!cJSON_IsObject(projection_graph))
         return config_issue("\"kb.curator.projection_graph\" expected object, got %s",
                             jo_type_name(projection_graph));
      const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(projection_graph, "enabled");
      if (enabled && cJSON_IsBool(enabled))
         cfg->kb_curator_projection_graph_enabled = cJSON_IsTrue(enabled) ? 1 : 0;
   }

   const cJSON *cross_repo = cJSON_GetObjectItemCaseSensitive(curator, "cross_repo_graph");
   if (cross_repo)
   {
      if (!cJSON_IsObject(cross_repo))
         return config_issue("\"kb.curator.cross_repo_graph\" expected object, got %s",
                             jo_type_name(cross_repo));
      const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(cross_repo, "enabled");
      if (enabled && cJSON_IsBool(enabled))
         cfg->kb_curator_cross_repo_graph_enabled = cJSON_IsTrue(enabled) ? 1 : 0;

      /* Positive-int knobs, each with an explicit upper bound to reject pathological
       * inputs (K=10M, timeout=1h, max_candidates=INT_MAX). Automatic (non-static)
       * so the &cfg->field initializers are valid at runtime. */
      const struct
      {
         const char *key;
         int *field;
         int max;
      } ints[] = {
          {"distinctiveness_v", &cfg->kb_curator_cross_repo_distinctiveness_v, 1000000},
          {"k", &cfg->kb_curator_cross_repo_k, 100000},
          {"m", &cfg->kb_curator_cross_repo_m, 100000},
          {"p_pct", &cfg->kb_curator_cross_repo_p_pct, 100},
          {"len_min", &cfg->kb_curator_cross_repo_len_min, 1024},
          {"caller_collision_c", &cfg->kb_curator_cross_repo_caller_collision_c, 100000},
          {"max_candidates", &cfg->kb_curator_cross_repo_max_candidates, 100000000},
          {"query_timeout_ms", &cfg->kb_curator_cross_repo_query_timeout_ms, 600000},
          {"review_queue_max", &cfg->kb_curator_cross_repo_review_queue_max, 100000000},
      };
      for (size_t i = 0; i < sizeof(ints) / sizeof(ints[0]); i++)
      {
         const cJSON *v = cJSON_GetObjectItemCaseSensitive(cross_repo, ints[i].key);
         if (!v)
            continue;
         if (!cJSON_IsNumber(v) || v->valueint <= 0 || v->valueint > ints[i].max)
            config_issue("\"kb.curator.cross_repo_graph.%s\" must be an integer in [1, %d]",
                         ints[i].key, ints[i].max);
         else
            *ints[i].field = v->valueint;
      }
   }

   const cJSON *synthesize = cJSON_GetObjectItemCaseSensitive(curator, "synthesize");
   if (synthesize)
   {
      if (!cJSON_IsObject(synthesize))
         return config_issue("\"kb.curator.synthesize\" expected object, got %s",
                             jo_type_name(synthesize));
      const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(synthesize, "enabled");
      if (enabled && cJSON_IsBool(enabled))
         cfg->kb_curator_synthesize_enabled = cJSON_IsTrue(enabled) ? 1 : 0;
      const cJSON *k = cJSON_GetObjectItemCaseSensitive(synthesize, "k");
      if (k)
      {
         if (!cJSON_IsNumber(k) || k->valueint <= 0)
            config_issue("\"kb.curator.synthesize.k\" must be a positive integer");
         else
            cfg->kb_curator_synthesize_k = k->valueint;
      }
   }

   const cJSON *promote = cJSON_GetObjectItemCaseSensitive(curator, "promote_entity");
   if (promote)
   {
      if (!cJSON_IsObject(promote))
         return config_issue("\"kb.curator.promote_entity\" expected object, got %s",
                             jo_type_name(promote));
      const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(promote, "enabled");
      if (enabled && cJSON_IsBool(enabled))
         cfg->kb_curator_promote_entity_enabled = cJSON_IsTrue(enabled) ? 1 : 0;
      const cJSON *minsrc = cJSON_GetObjectItemCaseSensitive(promote, "min_sources");
      if (minsrc)
      {
         if (!cJSON_IsNumber(minsrc) || minsrc->valueint <= 0)
            config_issue("\"kb.curator.promote_entity.min_sources\" must be a positive integer");
         else
            cfg->kb_curator_promote_min_sources = minsrc->valueint;
      }
   }

   const cJSON *eprompt = cJSON_GetObjectItemCaseSensitive(curator, "extract_prompt_version");
   if (eprompt && cJSON_IsString(eprompt) && eprompt->valuestring)
      snprintf(cfg->kb_curator_extract_prompt_version,
               sizeof(cfg->kb_curator_extract_prompt_version), "%s", eprompt->valuestring);

   const cJSON *notify_sock =
       cJSON_GetObjectItemCaseSensitive(curator, "invalidation_notify_socket");
   if (notify_sock && cJSON_IsString(notify_sock) && notify_sock->valuestring)
      snprintf(cfg->kb_curator_invalidation_notify_socket,
               sizeof(cfg->kb_curator_invalidation_notify_socket), "%s", notify_sock->valuestring);

   const cJSON *emodel = cJSON_GetObjectItemCaseSensitive(curator, "embed_model_version");
   if (emodel && cJSON_IsString(emodel) && emodel->valuestring)
      snprintf(cfg->kb_curator_embed_model_version, sizeof(cfg->kb_curator_embed_model_version),
               "%s", emodel->valuestring);

   const cJSON *extract_command = cJSON_GetObjectItemCaseSensitive(curator, "extract_command");
   if (extract_command && cJSON_IsString(extract_command) && extract_command->valuestring)
      snprintf(cfg->kb_curator_extract_command, sizeof(cfg->kb_curator_extract_command), "%s",
               extract_command->valuestring);

   const cJSON *stage_order = cJSON_GetObjectItemCaseSensitive(curator, "stage_order");
   if (stage_order && cJSON_IsString(stage_order) && stage_order->valuestring)
      snprintf(cfg->kb_curator_stage_order, sizeof(cfg->kb_curator_stage_order), "%s",
               stage_order->valuestring);

   const cJSON *user_presets = cJSON_GetObjectItemCaseSensitive(curator, "user_presets");
   if (user_presets && cJSON_IsString(user_presets) && user_presets->valuestring)
      snprintf(cfg->kb_curator_user_presets, sizeof(cfg->kb_curator_user_presets), "%s",
               user_presets->valuestring);

   const cJSON *custom_stages = cJSON_GetObjectItemCaseSensitive(curator, "custom_stages");
   if (custom_stages && cJSON_IsString(custom_stages) && custom_stages->valuestring)
      snprintf(cfg->kb_curator_custom_stages, sizeof(cfg->kb_curator_custom_stages), "%s",
               custom_stages->valuestring);

   const cJSON *judge_command = cJSON_GetObjectItemCaseSensitive(curator, "judge_command");
   if (judge_command && cJSON_IsString(judge_command) && judge_command->valuestring)
      snprintf(cfg->kb_curator_judge_command, sizeof(cfg->kb_curator_judge_command), "%s",
               judge_command->valuestring);

   const cJSON *synth_command = cJSON_GetObjectItemCaseSensitive(curator, "synthesize_command");
   if (synth_command && cJSON_IsString(synth_command) && synth_command->valuestring)
      snprintf(cfg->kb_curator_synthesize_command, sizeof(cfg->kb_curator_synthesize_command), "%s",
               synth_command->valuestring);

   /* The curator's synthesis provider (§2), under "provider". One provider for
    * every stage: the second one under "tier_b" went with the tiers. */
   parse_curator_provider(curator, "provider", cfg->kb_curator_provider_base_url,
                          sizeof(cfg->kb_curator_provider_base_url), cfg->kb_curator_provider_model,
                          sizeof(cfg->kb_curator_provider_model), cfg->kb_curator_provider_api_key,
                          sizeof(cfg->kb_curator_provider_api_key));

   const cJSON *max_tokens = cJSON_GetObjectItemCaseSensitive(curator, "extract_max_tokens");
   if (max_tokens)
   {
      if (!cJSON_IsNumber(max_tokens) || max_tokens->valueint <= 0)
         config_issue("\"kb.curator.extract_max_tokens\" must be a positive integer");
      else
         cfg->kb_curator_extract_max_tokens = max_tokens->valueint;
   }

   /* max_jobs_per_hour was the curator rate cap; removed (§5 — the drain now runs
    * until the backlog is drained, then idles). A leftover key in an existing
    * config is harmless: the kb.curator section is not schema-validated, so it is
    * silently ignored. */

   const cJSON *max_attempts = cJSON_GetObjectItemCaseSensitive(curator, "max_attempts");
   if (max_attempts)
   {
      if (!cJSON_IsNumber(max_attempts) || max_attempts->valueint <= 0)
         config_issue("\"kb.curator.max_attempts\" must be a positive integer");
      else
         cfg->kb_curator_max_attempts = max_attempts->valueint;
   }

   /* kb.evidence.embed.{enabled,batch} — evidence-vector embed drain. */
   const cJSON *evidence = cJSON_GetObjectItemCaseSensitive(kb, "evidence");
   if (evidence)
   {
      if (!cJSON_IsObject(evidence))
         return config_issue("\"kb.evidence\" expected object, got %s", jo_type_name(evidence));
      const cJSON *embed = cJSON_GetObjectItemCaseSensitive(evidence, "embed");
      if (embed)
      {
         if (!cJSON_IsObject(embed))
            return config_issue("\"kb.evidence.embed\" expected object, got %s",
                                jo_type_name(embed));
         const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(embed, "enabled");
         if (enabled && cJSON_IsBool(enabled))
            cfg->kb_evidence_embed_enabled = cJSON_IsTrue(enabled) ? 1 : 0;
         const cJSON *batch = cJSON_GetObjectItemCaseSensitive(embed, "batch");
         if (batch)
         {
            if (!cJSON_IsNumber(batch) || batch->valueint <= 0)
               config_issue("\"kb.evidence.embed.batch\" must be a positive integer");
            else
               cfg->kb_evidence_embed_batch = batch->valueint;
         }
      }
   }

   return 0;
}

/* Add a {"enabled": true} child under `cur` for an on-gate; no-op when off. */
static void kb_curator_save_gate(cJSON *cur, const char *name, int enabled)
{
   if (!enabled)
      return;
   cJSON *o = cJSON_AddObjectToObject(cur, name);
   if (o)
      cJSON_AddBoolToObject(o, "enabled", 1);
}

/* Inverse of parse_curator_provider: emit {base_url, model, api_key} under `name`
 * only when at least one field is set (a clean install writes nothing). */
static void kb_curator_save_provider(cJSON *cur, const char *name, const char *base_url,
                                     const char *model, const char *api_key)
{
   if (!base_url[0] && !model[0] && !api_key[0])
      return;
   cJSON *p = cJSON_AddObjectToObject(cur, name);
   if (!p)
      return;
   if (base_url[0])
      cJSON_AddStringToObject(p, "base_url", base_url);
   if (model[0])
      cJSON_AddStringToObject(p, "model", model);
   if (api_key[0])
      cJSON_AddStringToObject(p, "api_key", api_key);
}

/* Serialize the kb.curator.* and kb.evidence.embed.* config back into `root`,
 * the inverse of config_parse_kb_curator. Only emitted when something differs
 * from the defaults, so a clean install writes nothing. This is what lets the
 * curator gates survive a config_save round-trip (e.g. --bootstrap-db2). */
void config_save_kb_curator(const config_t *cfg, cJSON *root)
{
   int curator_any =
       cfg->kb_curator_extract_docs_enabled || cfg->kb_curator_extract_code_enabled ||
       cfg->kb_curator_resolve_entities_enabled || cfg->kb_curator_index_narrative_enabled ||
       cfg->kb_curator_index_claims_enabled || cfg->kb_curator_detect_contradictions_enabled ||
       cfg->kb_curator_index_code_unit_enabled || cfg->kb_curator_link_artifacts_enabled ||
       cfg->kb_curator_projection_graph_enabled || cfg->kb_curator_synthesize_enabled ||
       cfg->kb_curator_promote_entity_enabled || cfg->kb_curator_extract_command[0] ||
       cfg->kb_curator_judge_command[0] || cfg->kb_curator_synthesize_command[0] ||
       cfg->kb_curator_extract_max_tokens != 4096 || cfg->kb_curator_max_attempts != 3 ||
       cfg->kb_curator_synthesize_k != 8 || cfg->kb_curator_promote_min_sources != 3 ||
       cfg->kb_curator_provider_base_url[0] || cfg->kb_curator_provider_model[0];
   int cross_repo_nondefault =
       !cfg->kb_curator_cross_repo_graph_enabled ||
       cfg->kb_curator_cross_repo_distinctiveness_v != 1 || cfg->kb_curator_cross_repo_k != 5 ||
       cfg->kb_curator_cross_repo_m != 8 || cfg->kb_curator_cross_repo_p_pct != 25 ||
       cfg->kb_curator_cross_repo_len_min != 4 ||
       cfg->kb_curator_cross_repo_caller_collision_c != 5 ||
       cfg->kb_curator_cross_repo_max_candidates != 50000 ||
       cfg->kb_curator_cross_repo_query_timeout_ms != 5000 ||
       cfg->kb_curator_cross_repo_review_queue_max != 5000;
   /* The preset itself is non-default state: a non-"full" tier must persist even
    * when it turns every stage OFF (in which case the gate writes below emit
    * nothing). Parse applies the tier first, so on reload it re-derives the gates. */
   int tier_nondefault = cfg->kb_curator_tier[0] && strcmp(cfg->kb_curator_tier, "full") != 0;
   curator_any = curator_any || cross_repo_nondefault || tier_nondefault;
   int evidence_any = !cfg->kb_evidence_embed_enabled || cfg->kb_evidence_embed_batch != 32;
   int typed_facts_any = cfg->typed_facts_enabled || !cfg->kb_typed_facts_auto_promote_enabled ||
                         cfg->kb_typed_facts_promote_threshold != 3;
   if (!curator_any && !evidence_any && !typed_facts_any)
      return;

   cJSON *kb = cJSON_GetObjectItemCaseSensitive(root, "kb");
   if (!kb)
      kb = cJSON_AddObjectToObject(root, "kb");
   if (!kb)
      return;

   /* kb.typed_facts.* — autonomous reconciliation knobs (§7.2/§8). auto_promote is
    * default-on, so emit it whenever anything here is non-default; promote_threshold
    * only when it differs from the built-in, so a clean install writes nothing. */
   if (typed_facts_any)
   {
      cJSON *tf = cJSON_AddObjectToObject(kb, "typed_facts");
      if (tf)
      {
         /* KB-owned master gate — the sole persisted home for typed_facts_enabled
          * (the legacy root-level emit is removed in config_save.c). */
         cJSON_AddBoolToObject(tf, "enabled", cfg->typed_facts_enabled ? 1 : 0);
         cJSON_AddBoolToObject(tf, "auto_promote",
                               cfg->kb_typed_facts_auto_promote_enabled ? 1 : 0);
         if (cfg->kb_typed_facts_promote_threshold != 3)
            cJSON_AddNumberToObject(tf, "promote_threshold", cfg->kb_typed_facts_promote_threshold);
      }
   }

   if (curator_any)
   {
      cJSON *cur = cJSON_AddObjectToObject(kb, "curator");
      if (cur)
      {
         if (tier_nondefault)
            cJSON_AddStringToObject(cur, "tier", cfg->kb_curator_tier);
         kb_curator_save_gate(cur, "extract_docs", cfg->kb_curator_extract_docs_enabled);
         kb_curator_save_gate(cur, "extract_code", cfg->kb_curator_extract_code_enabled);
         kb_curator_save_gate(cur, "resolve_entities", cfg->kb_curator_resolve_entities_enabled);
         kb_curator_save_gate(cur, "index_narrative", cfg->kb_curator_index_narrative_enabled);
         kb_curator_save_gate(cur, "index_claims", cfg->kb_curator_index_claims_enabled);
         kb_curator_save_gate(cur, "detect_contradictions",
                              cfg->kb_curator_detect_contradictions_enabled);
         kb_curator_save_gate(cur, "index_code_unit", cfg->kb_curator_index_code_unit_enabled);
         kb_curator_save_gate(cur, "link_artifacts", cfg->kb_curator_link_artifacts_enabled);
         kb_curator_save_gate(cur, "projection_graph", cfg->kb_curator_projection_graph_enabled);

         if (cfg->kb_curator_synthesize_enabled || cfg->kb_curator_synthesize_k != 8)
         {
            cJSON *o = cJSON_AddObjectToObject(cur, "synthesize");
            if (o)
            {
               cJSON_AddBoolToObject(o, "enabled", cfg->kb_curator_synthesize_enabled ? 1 : 0);
               if (cfg->kb_curator_synthesize_k != 8)
                  cJSON_AddNumberToObject(o, "k", cfg->kb_curator_synthesize_k);
            }
         }
         if (cfg->kb_curator_promote_entity_enabled || cfg->kb_curator_promote_min_sources != 3)
         {
            cJSON *o = cJSON_AddObjectToObject(cur, "promote_entity");
            if (o)
            {
               cJSON_AddBoolToObject(o, "enabled", cfg->kb_curator_promote_entity_enabled ? 1 : 0);
               if (cfg->kb_curator_promote_min_sources != 3)
                  cJSON_AddNumberToObject(o, "min_sources", cfg->kb_curator_promote_min_sources);
            }
         }
         if (cross_repo_nondefault)
         {
            cJSON *o = cJSON_AddObjectToObject(cur, "cross_repo_graph");
            if (o)
            {
               cJSON_AddBoolToObject(o, "enabled",
                                     cfg->kb_curator_cross_repo_graph_enabled ? 1 : 0);
               if (cfg->kb_curator_cross_repo_distinctiveness_v != 1)
                  cJSON_AddNumberToObject(o, "distinctiveness_v",
                                          cfg->kb_curator_cross_repo_distinctiveness_v);
               if (cfg->kb_curator_cross_repo_k != 5)
                  cJSON_AddNumberToObject(o, "k", cfg->kb_curator_cross_repo_k);
               if (cfg->kb_curator_cross_repo_m != 8)
                  cJSON_AddNumberToObject(o, "m", cfg->kb_curator_cross_repo_m);
               if (cfg->kb_curator_cross_repo_p_pct != 25)
                  cJSON_AddNumberToObject(o, "p_pct", cfg->kb_curator_cross_repo_p_pct);
               if (cfg->kb_curator_cross_repo_len_min != 4)
                  cJSON_AddNumberToObject(o, "len_min", cfg->kb_curator_cross_repo_len_min);
               if (cfg->kb_curator_cross_repo_caller_collision_c != 5)
                  cJSON_AddNumberToObject(o, "caller_collision_c",
                                          cfg->kb_curator_cross_repo_caller_collision_c);
               if (cfg->kb_curator_cross_repo_max_candidates != 50000)
                  cJSON_AddNumberToObject(o, "max_candidates",
                                          cfg->kb_curator_cross_repo_max_candidates);
               if (cfg->kb_curator_cross_repo_query_timeout_ms != 5000)
                  cJSON_AddNumberToObject(o, "query_timeout_ms",
                                          cfg->kb_curator_cross_repo_query_timeout_ms);
               if (cfg->kb_curator_cross_repo_review_queue_max != 5000)
                  cJSON_AddNumberToObject(o, "review_queue_max",
                                          cfg->kb_curator_cross_repo_review_queue_max);
            }
         }
         if (cfg->kb_curator_extract_command[0])
            cJSON_AddStringToObject(cur, "extract_command", cfg->kb_curator_extract_command);
         if (cfg->kb_curator_judge_command[0])
            cJSON_AddStringToObject(cur, "judge_command", cfg->kb_curator_judge_command);
         if (cfg->kb_curator_synthesize_command[0])
            cJSON_AddStringToObject(cur, "synthesize_command", cfg->kb_curator_synthesize_command);
         if (cfg->kb_curator_extract_max_tokens != 4096)
            cJSON_AddNumberToObject(cur, "extract_max_tokens", cfg->kb_curator_extract_max_tokens);
         if (cfg->kb_curator_max_attempts != 3)
            cJSON_AddNumberToObject(cur, "max_attempts", cfg->kb_curator_max_attempts);
         kb_curator_save_provider(cur, "provider", cfg->kb_curator_provider_base_url,
                                  cfg->kb_curator_provider_model, "");
      }
   }

   if (evidence_any)
   {
      cJSON *evi = cJSON_AddObjectToObject(kb, "evidence");
      if (evi)
      {
         cJSON *emb = cJSON_AddObjectToObject(evi, "embed");
         if (emb)
         {
            cJSON_AddBoolToObject(emb, "enabled", cfg->kb_evidence_embed_enabled ? 1 : 0);
            if (cfg->kb_evidence_embed_batch != 32)
               cJSON_AddNumberToObject(emb, "batch", cfg->kb_evidence_embed_batch);
         }
      }
   }
}
