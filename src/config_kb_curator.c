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
 *     extract_max_tokens: 2048
 *     max_attempts: 3
 */

#include "aimee.h"
#include "config.h"
#include "json_fluent.h"
#include "cJSON.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void config_kb_curator_defaults(config_t *cfg)
{
   /* The deep-curator (larger LLM) pipeline is default-ON: it drains the backlog
    * continuously in the background (no artificial rate cap) and refines what the
    * 0.6B embedder already indexed. Every stage degrades to a no-op when its
    * prerequisite (a configured curator/judge/synthesize command, or upstream
    * curator rows) is absent, so default-on is safe on a bare deploy. */
   cfg->kb_curator_extract_docs_enabled = 1;
   cfg->kb_curator_extract_prompt_version[0] = '\0';
   cfg->kb_curator_embed_model_version[0] = '\0';
   cfg->kb_curator_invalidation_notify_socket[0] = '\0';
   cfg->kb_curator_extract_code_enabled = 1;
   cfg->kb_curator_resolve_entities_enabled = 1;
   cfg->kb_curator_index_narrative_enabled = 1;
   cfg->kb_curator_index_claims_enabled = 1;
   cfg->kb_curator_detect_contradictions_enabled = 1;
   cfg->kb_curator_index_code_unit_enabled = 1;
   cfg->kb_curator_link_artifacts_enabled = 1;
   cfg->kb_curator_synthesize_enabled = 1;
   cfg->kb_curator_promote_entity_enabled = 1;
   cfg->kb_curator_promote_min_sources = 3;
   cfg->kb_curator_extract_command[0] = '\0';
   cfg->kb_curator_judge_command[0] = '\0';
   cfg->kb_curator_synthesize_command[0] = '\0';
   cfg->kb_curator_synthesize_k = 8;
   cfg->kb_curator_extract_max_tokens = 2048;
   cfg->kb_curator_max_attempts = 3;
   cfg->kb_evidence_embed_enabled = 1;
   cfg->kb_evidence_embed_batch = 32;
}

int config_parse_kb_curator(config_t *cfg, const cJSON *root)
{
   const cJSON *kb = cJSON_GetObjectItemCaseSensitive(root, "kb");
   if (!kb)
      return 0;
   if (!cJSON_IsObject(kb))
      return config_issue("\"kb\" expected object, got %s", jo_type_name(kb));

   const cJSON *curator = cJSON_GetObjectItemCaseSensitive(kb, "curator");
   if (!curator)
      return 0;
   if (!cJSON_IsObject(curator))
      return config_issue("\"kb.curator\" expected object, got %s", jo_type_name(curator));

   const cJSON *extract_docs = cJSON_GetObjectItemCaseSensitive(curator, "extract_docs");
   if (extract_docs)
   {
      if (!cJSON_IsObject(extract_docs))
         return config_issue("\"kb.curator.extract_docs\" expected object, got %s",
                             jo_type_name(extract_docs));
      const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(extract_docs, "enabled");
      if (enabled && cJSON_IsBool(enabled))
         cfg->kb_curator_extract_docs_enabled = cJSON_IsTrue(enabled) ? 1 : 0;
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

   const cJSON *judge_command = cJSON_GetObjectItemCaseSensitive(curator, "judge_command");
   if (judge_command && cJSON_IsString(judge_command) && judge_command->valuestring)
      snprintf(cfg->kb_curator_judge_command, sizeof(cfg->kb_curator_judge_command), "%s",
               judge_command->valuestring);

   const cJSON *synth_command = cJSON_GetObjectItemCaseSensitive(curator, "synthesize_command");
   if (synth_command && cJSON_IsString(synth_command) && synth_command->valuestring)
      snprintf(cfg->kb_curator_synthesize_command, sizeof(cfg->kb_curator_synthesize_command), "%s",
               synth_command->valuestring);

   const cJSON *max_tokens = cJSON_GetObjectItemCaseSensitive(curator, "extract_max_tokens");
   if (max_tokens)
   {
      if (!cJSON_IsNumber(max_tokens) || max_tokens->valueint <= 0)
         config_issue("\"kb.curator.extract_max_tokens\" must be a positive integer");
      else
         cfg->kb_curator_extract_max_tokens = max_tokens->valueint;
   }

   /* max_jobs_per_hour was the curator rate cap; removed (§5 — the drain now runs
    * to backlog and idles). A leftover key in an existing config is harmless: the
    * kb.curator section is not schema-validated, so it is silently ignored. */

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
       cfg->kb_curator_synthesize_enabled || cfg->kb_curator_promote_entity_enabled ||
       cfg->kb_curator_extract_command[0] || cfg->kb_curator_judge_command[0] ||
       cfg->kb_curator_synthesize_command[0] || cfg->kb_curator_extract_max_tokens != 2048 ||
       cfg->kb_curator_max_attempts != 3 || cfg->kb_curator_synthesize_k != 8 ||
       cfg->kb_curator_promote_min_sources != 3;
   int evidence_any = !cfg->kb_evidence_embed_enabled || cfg->kb_evidence_embed_batch != 32;
   if (!curator_any && !evidence_any)
      return;

   cJSON *kb = cJSON_GetObjectItemCaseSensitive(root, "kb");
   if (!kb)
      kb = cJSON_AddObjectToObject(root, "kb");
   if (!kb)
      return;

   if (curator_any)
   {
      cJSON *cur = cJSON_AddObjectToObject(kb, "curator");
      if (cur)
      {
         kb_curator_save_gate(cur, "extract_docs", cfg->kb_curator_extract_docs_enabled);
         kb_curator_save_gate(cur, "extract_code", cfg->kb_curator_extract_code_enabled);
         kb_curator_save_gate(cur, "resolve_entities", cfg->kb_curator_resolve_entities_enabled);
         kb_curator_save_gate(cur, "index_narrative", cfg->kb_curator_index_narrative_enabled);
         kb_curator_save_gate(cur, "index_claims", cfg->kb_curator_index_claims_enabled);
         kb_curator_save_gate(cur, "detect_contradictions",
                              cfg->kb_curator_detect_contradictions_enabled);
         kb_curator_save_gate(cur, "index_code_unit", cfg->kb_curator_index_code_unit_enabled);
         kb_curator_save_gate(cur, "link_artifacts", cfg->kb_curator_link_artifacts_enabled);

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
         if (cfg->kb_curator_extract_command[0])
            cJSON_AddStringToObject(cur, "extract_command", cfg->kb_curator_extract_command);
         if (cfg->kb_curator_judge_command[0])
            cJSON_AddStringToObject(cur, "judge_command", cfg->kb_curator_judge_command);
         if (cfg->kb_curator_synthesize_command[0])
            cJSON_AddStringToObject(cur, "synthesize_command", cfg->kb_curator_synthesize_command);
         if (cfg->kb_curator_extract_max_tokens != 2048)
            cJSON_AddNumberToObject(cur, "extract_max_tokens", cfg->kb_curator_extract_max_tokens);
         if (cfg->kb_curator_max_attempts != 3)
            cJSON_AddNumberToObject(cur, "max_attempts", cfg->kb_curator_max_attempts);
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
