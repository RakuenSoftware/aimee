/* learning_evidence.c: evidence capture into the charter artifact pipeline.
 * See docs/proposals/done/cross-source-learning-substrate.md */

#include "learning_evidence.h"
#include "aimee.h" /* memory_directive_t and friends, TIER_* */
#include "db2/artifacts.h"
#include "db2/anti_patterns.h"
#include "db2/workflow_patterns.h"
#include "db2/rules.h"
#include "db2/epistemic_directives.h"
#include "db2/entity_nodes.h"
#include "db2/evidence_vectors.h"
#include "db2/learning_synth_ops.h"
#include "db2/kb_service_backend.h"
#include "db2/demotion.h"
#include "log.h"
#include "cJSON.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Borderline window below the promotion threshold: candidates here still
 * auto-apply but the audit row is flagged_for_review. */
#define LEARNING_PROMOTE_BORDERLINE 0.10

/* Build a minimal JSON payload for a feedback artifact. */
static void build_feedback_payload(const char *polarity, const char *title, const char *description,
                                   char *buf, size_t len)
{
   const char *desc = description ? description : "";
   snprintf(buf, len, "{\"polarity\":\"%s\",\"title\":\"%s\",\"description\":\"%s\"}",
            polarity ? polarity : "", title ? title : "", desc);
}

/* FNV-1a content hash, hex-encoded — the idempotency key for evidence capture.
 * Self-contained so capture pulls in no crypto/link dependency. */
static void evidence_content_hash(const char *kind, const char *content, char out[17])
{
   uint64_t h = 1469598103934665603ULL; /* FNV offset basis */
   for (const char *p = kind; p && *p; p++)
      h = (h ^ (unsigned char)*p) * 1099511628211ULL;
   h = (h ^ 0x1f) * 1099511628211ULL; /* domain separator between kind and content */
   for (const char *p = content; p && *p; p++)
      h = (h ^ (unsigned char)*p) * 1099511628211ULL;
   snprintf(out, 17, "%016llx", (unsigned long long)h);
}

/* The non-doc charter evidence kinds this capture path admits. Doc-side kinds
 * (doc_summary, claim, ...) are owned by the deep-curator extraction passes. */
static int evidence_kind_allowed(const char *kind)
{
   static const char *const kinds[] = {
       "session_turn", "session_summary", "feedback_positive", "feedback_negative",
       "tool_outcome", "guardrail_event", "benchmark_trace",   "workflow_pattern",
   };
   if (!kind)
      return 0;
   for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
      if (strcmp(kind, kinds[i]) == 0)
         return 1;
   return 0;
}

/* JSON-escape a string into buf (bounded). Minimal: handles ", \\, and control
 * chars so evidence content can't break the payload. */
static void json_escape_into(const char *src, char *buf, size_t len)
{
   size_t o = 0;
   for (const char *p = src ? src : ""; *p && o + 7 < len; p++)
   {
      unsigned char c = (unsigned char)*p;
      if (c == '"' || c == '\\')
      {
         buf[o++] = '\\';
         buf[o++] = (char)c;
      }
      else if (c < 0x20)
         o += (size_t)snprintf(buf + o, len - o, "\\u%04x", c);
      else
         buf[o++] = (char)c;
   }
   buf[o] = '\0';
}

int learning_evidence_write_event(const char *source_kind, const char *scope_kind,
                                  const char *scope_id, const char *content,
                                  const char *operator_id, char *artifact_id_out,
                                  int artifact_id_out_len)
{
   if (artifact_id_out && artifact_id_out_len > 0)
      artifact_id_out[0] = '\0';

   if (!evidence_kind_allowed(source_kind) || !content)
      return -1;

   char content_hash[17];
   evidence_content_hash(source_kind, content, content_hash);

   char esc[1024];
   json_escape_into(content, esc, sizeof(esc));

   char payload[1280];
   snprintf(payload, sizeof(payload),
            "{\"source_kind\":\"%s\",\"scope_kind\":\"%s\",\"scope_id\":\"%s\","
            "\"content_hash\":\"%s\",\"content\":\"%s\"}",
            source_kind, scope_kind ? scope_kind : "user", scope_id ? scope_id : "", content_hash,
            esc);

   char id[37] = "";
   int rc = db2_artifact_write_evidence(source_kind, scope_kind, scope_id, operator_id,
                                        content_hash, payload, id, sizeof(id));
   if (rc != 0)
   {
      aimee_log(LOG_DEBUG, "learning_evidence", "evidence write failed (kind=%s)", source_kind);
      return -1;
   }

   /* Enqueue the evidence row for embedding into evidence_vectors (idempotent;
    * the embed worker fills the vector asynchronously). A plain DB insert — no
    * LLM on the capture hot path. */
   if (id[0])
   {
      (void)db2_evidence_enqueue(id, "evidence");
      /* Also enqueue it for candidate synthesis — drained on the kb scheduler,
       * so the LLM pass never touches this capture hot path. */
      (void)db2_synth_enqueue(id);
   }

   if (artifact_id_out && artifact_id_out_len > 0)
      snprintf(artifact_id_out, (size_t)artifact_id_out_len, "%s", id);

   aimee_log(LOG_DEBUG, "learning_evidence", "evidence artifact: kind=%s hash=%s", source_kind,
             content_hash);
   return 0;
}

/* Copy a string field of payload into out (first non-empty of keys[]). */
static void payload_first_string(const cJSON *payload, const char *const *keys, int nkeys,
                                 char *out, size_t len)
{
   out[0] = '\0';
   for (int i = 0; i < nkeys; i++)
   {
      const cJSON *v = cJSON_GetObjectItemCaseSensitive(payload, keys[i]);
      if (cJSON_IsString(v) && v->valuestring[0])
      {
         snprintf(out, len, "%s", v->valuestring);
         return;
      }
   }
}

/* Write the charter audit_events row recording a promotion to `surface` with
 * `target_id` (the surface row's natural id). Returns 0 on success, -1 on error. */
static int promote_emit_audit(const db2_artifact_row_t *art, const char *surface,
                              const char *target_id, int flagged)
{
   char audit_id[37];
   db2_artifact_gen_id(audit_id, sizeof(audit_id));
   char after_json[256];
   snprintf(after_json, sizeof(after_json), "{\"surface\":\"%s\",\"target_id\":\"%s\"}", surface,
            target_id);
   return db2_audit_event_write(audit_id, art->id, surface, target_id, "kb.learning.promote",
                                art->scope_kind, art->scope_id, art->confidence, flagged,
                                "{\"surface\":null}", after_json);
}

/* Promote a committed anti_pattern/mistake_pattern candidate into the
 * anti_patterns target surface. */
static int promote_anti_pattern(const db2_artifact_row_t *art, double threshold, int flagged)
{
   cJSON *pl = cJSON_Parse(art->payload_json);
   char pattern[512] = "", description[1024] = "";
   if (pl)
   {
      static const char *const pat_keys[] = {"pattern", "tag", "title", "content"};
      static const char *const desc_keys[] = {"description", "content"};
      payload_first_string(pl, pat_keys, 4, pattern, sizeof(pattern));
      payload_first_string(pl, desc_keys, 2, description, sizeof(description));
      cJSON_Delete(pl);
   }
   if (!pattern[0])
      snprintf(pattern, sizeof(pattern), "%s", art->kind);

   anti_pattern_t ap;
   memset(&ap, 0, sizeof(ap));
   if (db2_anti_pattern_insert(pattern, description, "kb.learning.promote", art->id,
                               art->confidence, &ap) != 0)
      return -1;

   char audit_id[37];
   db2_artifact_gen_id(audit_id, sizeof(audit_id));
   char target_id[32];
   snprintf(target_id, sizeof(target_id), "%lld", (long long)ap.id);
   char after_json[160];
   snprintf(after_json, sizeof(after_json), "{\"surface\":\"anti_pattern\",\"row_id\":%lld}",
            (long long)ap.id);
   (void)threshold;
   if (db2_audit_event_write(audit_id, art->id, "anti_pattern", target_id, "kb.learning.promote",
                             art->scope_kind, art->scope_id, art->confidence, flagged,
                             "{\"surface\":null}", after_json) != 0)
      return -1;
   return 1;
}

/* Promote a committed preference candidate into the memory target surface,
 * through the canonical typed-verb store path (no bespoke memory mutation). */
static int promote_memory(const db2_artifact_row_t *art, int flagged)
{
   cJSON *pl = cJSON_Parse(art->payload_json);
   char key[256] = "", content[1024] = "";
   if (pl)
   {
      static const char *const key_keys[] = {"key", "tag", "title"};
      static const char *const content_keys[] = {"content", "description", "narrative"};
      payload_first_string(pl, key_keys, 3, key, sizeof(key));
      payload_first_string(pl, content_keys, 3, content, sizeof(content));
      cJSON_Delete(pl);
   }
   if (!key[0])
      snprintf(key, sizeof(key), "%s", art->id);
   if (!content[0])
      snprintf(content, sizeof(content), "%s", key);

   /* Durable tier; routes through db2_kb_service_memory_insert_json -> the
    * memory_insert typed-verb "store" path defined by memory-public-contract. */
   cJSON *resp =
       db2_kb_service_memory_insert_json(TIER_L1, "preference", key, content, art->confidence, "");
   if (!resp)
      return -1;
   const cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   const cJSON *idj = cJSON_GetObjectItemCaseSensitive(resp, "id");
   int ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
   long long mem_id = cJSON_IsNumber(idj) ? (long long)idj->valuedouble : -1;
   cJSON_Delete(resp);
   if (!ok)
      return -1;

   char audit_id[37];
   db2_artifact_gen_id(audit_id, sizeof(audit_id));
   char target_id[32];
   snprintf(target_id, sizeof(target_id), "%lld", mem_id);
   char after_json[160];
   snprintf(after_json, sizeof(after_json), "{\"surface\":\"memory\",\"memory_id\":%lld}", mem_id);
   if (db2_audit_event_write(audit_id, art->id, "memory", target_id, "kb.learning.promote",
                             art->scope_kind, art->scope_id, art->confidence, flagged,
                             "{\"surface\":null}", after_json) != 0)
      return -1;
   return 1;
}

/* Promote a committed workflow candidate into the workflow_patterns surface. */
static int promote_workflow_pattern(const db2_artifact_row_t *art, int flagged)
{
   cJSON *pl = cJSON_Parse(art->payload_json);
   char pattern[512] = "", description[1024] = "";
   if (pl)
   {
      static const char *const pat_keys[] = {"pattern", "tag", "title", "content"};
      static const char *const desc_keys[] = {"description", "content"};
      payload_first_string(pl, pat_keys, 4, pattern, sizeof(pattern));
      payload_first_string(pl, desc_keys, 2, description, sizeof(description));
      cJSON_Delete(pl);
   }
   if (!pattern[0])
      snprintf(pattern, sizeof(pattern), "%s", art->kind);

   workflow_pattern_t wp;
   memset(&wp, 0, sizeof(wp));
   if (db2_workflow_pattern_insert(pattern, description, "kb.learning.promote", art->id,
                                   art->confidence, &wp) != 0)
      return -1;

   char audit_id[37];
   db2_artifact_gen_id(audit_id, sizeof(audit_id));
   char target_id[32];
   snprintf(target_id, sizeof(target_id), "%lld", (long long)wp.id);
   char after_json[160];
   snprintf(after_json, sizeof(after_json), "{\"surface\":\"workflow_pattern\",\"row_id\":%lld}",
            (long long)wp.id);
   if (db2_audit_event_write(audit_id, art->id, "workflow_pattern", target_id,
                             "kb.learning.promote", art->scope_kind, art->scope_id, art->confidence,
                             flagged, "{\"surface\":null}", after_json) != 0)
      return -1;
   return 1;
}

/* Promote a committed candidate into the rules surface (active rules table). */
static int promote_rule(const db2_artifact_row_t *art, int flagged)
{
   cJSON *pl = cJSON_Parse(art->payload_json);
   char title[256] = "", description[1024] = "";
   if (pl)
   {
      static const char *const title_keys[] = {"title", "rule", "pattern", "key"};
      static const char *const desc_keys[] = {"description", "content"};
      payload_first_string(pl, title_keys, 4, title, sizeof(title));
      payload_first_string(pl, desc_keys, 2, description, sizeof(description));
      cJSON_Delete(pl);
   }
   if (!title[0])
      snprintf(title, sizeof(title), "%s", art->id);

   int weight = (int)(art->confidence * 100.0);
   if (db2_rules_insert("positive", title, description, weight) != 0)
      return -1;
   return promote_emit_audit(art, "rule", title, flagged) == 0 ? 1 : -1;
}

/* Promote a committed candidate into the epistemic-directives surface. */
static int promote_epistemic_directive(const db2_artifact_row_t *art, int flagged)
{
   cJSON *pl = cJSON_Parse(art->payload_json);
   char question[512] = "", topic[256] = "";
   if (pl)
   {
      static const char *const q_keys[] = {"question", "content", "title"};
      static const char *const topic_keys[] = {"topic", "tag"};
      payload_first_string(pl, q_keys, 3, question, sizeof(question));
      payload_first_string(pl, topic_keys, 2, topic, sizeof(topic));
      cJSON_Delete(pl);
   }
   if (!question[0])
      snprintf(question, sizeof(question), "%s", art->id);
   if (!topic[0])
      snprintf(topic, sizeof(topic), "%s", art->scope_id);

   int64_t dir_id = 0;
   int existed = 0;
   if (db2_directive_insert_ignore(question, topic, "", "", "promoted_directive", 50, 0, 0, "", "",
                                   "", &dir_id, &existed) != 0)
      return -1;
   char target_id[32];
   snprintf(target_id, sizeof(target_id), "%lld", (long long)dir_id);
   return promote_emit_audit(art, "epistemic_directive", target_id, flagged) == 0 ? 1 : -1;
}

/* Promote a committed candidate into the entity-alias surface. */
static int promote_entity(const db2_artifact_row_t *art, int flagged)
{
   cJSON *pl = cJSON_Parse(art->payload_json);
   char alias[256] = "", node_key[256] = "";
   if (pl)
   {
      static const char *const alias_keys[] = {"alias", "mention", "title", "key"};
      static const char *const node_keys[] = {"node_key", "canonical", "entity"};
      payload_first_string(pl, alias_keys, 4, alias, sizeof(alias));
      payload_first_string(pl, node_keys, 3, node_key, sizeof(node_key));
      cJSON_Delete(pl);
   }
   if (!alias[0])
      snprintf(alias, sizeof(alias), "%s", art->id);
   if (!node_key[0])
      snprintf(node_key, sizeof(node_key), "%s", alias);

   const char *project = art->scope_id[0] ? art->scope_id : "";
   /* Ensure the canonical node exists (the alias FKs to it), then attach the
    * learned alias mapping to it. */
   if (db2_entity_node_upsert(node_key, 0, project, alias, node_key, "", "", "learned", 0) != 0)
      return -1;
   if (db2_entity_node_alias_upsert(alias, node_key, "learned", project, 0) != 0)
      return -1;
   return promote_emit_audit(art, "entity", node_key, flagged) == 0 ? 1 : -1;
}

/* Promote a committed candidate into the guardrail-exemplar surface (register
 * the artifact as a case exemplar; its vector is embedded asynchronously). */
static int promote_guardrail_exemplar(const db2_artifact_row_t *art, int flagged)
{
   if (db2_artifact_register_exemplar(art->id, "case_exemplars") != 0)
      return -1;
   return promote_emit_audit(art, "guardrail_exemplar", art->id, flagged) == 0 ? 1 : -1;
}

/* The working-profile field writer lives in DB1 (db1/working_profile_local.c).
 * Declared weak so this object links into the DB1-less aimee-kb binary; there
 * the symbol resolves to NULL and working_profile promotion no-ops (it only runs
 * in the DB1-owning process). The full binary and unit tests provide it. */
extern int db1_working_profile_local_observe(const char *field, const char *value,
                                             double confidence, const char *session_id,
                                             int threshold) __attribute__((weak));

/* Promote a committed candidate into the working_profile surface, through the
 * DB1 working-profile field observer (no bespoke DB1 write). */
static int promote_working_profile(const db2_artifact_row_t *art, int flagged)
{
   if (!db1_working_profile_local_observe)
      return -2; /* DB1 not present in this process (e.g. aimee-kb) */

   cJSON *pl = cJSON_Parse(art->payload_json);
   char field[128] = "", value[512] = "";
   if (pl)
   {
      static const char *const field_keys[] = {"field", "key", "title"};
      static const char *const value_keys[] = {"value", "content", "description"};
      payload_first_string(pl, field_keys, 3, field, sizeof(field));
      payload_first_string(pl, value_keys, 3, value, sizeof(value));
      cJSON_Delete(pl);
   }
   if (!field[0])
      return -1;

   if (db1_working_profile_local_observe(field, value, art->confidence, "", 1) != 0)
      return -1;
   return promote_emit_audit(art, "working_profile", field, flagged) == 0 ? 1 : -1;
}

int learning_promote(const char *candidate_id, double threshold)
{
   if (!candidate_id || !candidate_id[0])
      return -1;

   db2_artifact_row_t art;
   if (db2_artifact_read(candidate_id, &art, NULL, 0, NULL) != 0)
      return -1;
   if (strcmp(art.state, "committed") != 0)
      return -1; /* only committed candidates promote */

   /* The artifact's explicit target_surface column wins (set by candidate
    * generation, keyed per the charter promotion table); otherwise derive it
    * from the candidate kind for the substrate's own four kinds. */
   char ts[64] = "";
   db2_artifact_target_surface(candidate_id, ts, sizeof(ts));
   const char *surface = ts[0] ? ts : learning_candidate_target_surface(art.kind);
   if (!surface || !surface[0])
      return -1;

   /* Below the borderline window: hold for review, do not auto-apply. */
   if (art.confidence < threshold - LEARNING_PROMOTE_BORDERLINE)
      return 0;
   int flagged = (art.confidence < threshold) ? 1 : 0;

   int rc;
   if (strcmp(surface, "anti_pattern") == 0)
      rc = promote_anti_pattern(&art, threshold, flagged);
   else if (strcmp(surface, "memory") == 0)
      rc = promote_memory(&art, flagged);
   else if (strcmp(surface, "workflow_pattern") == 0)
      rc = promote_workflow_pattern(&art, flagged);
   else if (strcmp(surface, "rule") == 0)
      rc = promote_rule(&art, flagged);
   else if (strcmp(surface, "epistemic_directive") == 0)
      rc = promote_epistemic_directive(&art, flagged);
   else if (strcmp(surface, "entity") == 0)
      rc = promote_entity(&art, flagged);
   else if (strcmp(surface, "guardrail_exemplar") == 0)
      rc = promote_guardrail_exemplar(&art, flagged);
   else if (strcmp(surface, "working_profile") == 0)
      rc = promote_working_profile(&art, flagged);
   else
      return -2; /* unknown target surface */

   if (rc == 1)
      aimee_log(LOG_DEBUG, "learning_evidence", "promoted %s -> %s (flagged=%d)", candidate_id,
                surface, flagged);
   return rc;
}

const char *learning_candidate_target_surface(const char *candidate_kind)
{
   if (!candidate_kind)
      return NULL;
   if (strcmp(candidate_kind, "preference") == 0)
      return "memory";
   if (strcmp(candidate_kind, "workflow") == 0)
      return "workflow_pattern";
   if (strcmp(candidate_kind, "anti_pattern") == 0)
      return "anti_pattern";
   if (strcmp(candidate_kind, "mistake_pattern") == 0)
      return "anti_pattern"; /* mistakes escalate into the anti-pattern surface */
   return NULL;
}

/* Extract the "state" string value from a small {"state":"..."} JSON snapshot.
 * Returns 1 and fills out on success, 0 if absent. */
static int snapshot_state(const char *json, char *out, size_t len)
{
   if (out && len)
      out[0] = '\0';
   if (!json)
      return 0;
   const char *p = strstr(json, "\"state\"");
   if (!p)
      return 0;
   p = strchr(p + 7, ':');
   if (!p)
      return 0;
   p = strchr(p, '"');
   if (!p)
      return 0;
   p++;
   size_t o = 0;
   while (*p && *p != '"' && o + 1 < len)
      out[o++] = *p++;
   out[o] = '\0';
   return o > 0;
}

int learning_review_rollback(const char *candidate_id, const char *verdict_tag,
                             const char *verdict_scope, const char *counter_example)
{
   if (!candidate_id || !candidate_id[0])
      return -1;

   /* Roll back to the state captured in the candidate's most recent audit
    * before_snapshot; default to 'proposed' if none is recorded. */
   char before_json[128] = "";
   db2_audit_read_latest_before(candidate_id, before_json, sizeof(before_json));
   char restore_state[32] = "";
   if (!snapshot_state(before_json, restore_state, sizeof(restore_state)) || !restore_state[0])
      snprintf(restore_state, sizeof(restore_state), "proposed");

   int rc = db2_artifact_review_rollback(candidate_id, restore_state, verdict_tag, verdict_scope,
                                         counter_example);
   if (rc != 0)
   {
      aimee_log(LOG_DEBUG, "learning_evidence", "review rollback failed for %s", candidate_id);
      return -1;
   }
   aimee_log(LOG_DEBUG, "learning_evidence", "review rolled back %s -> %s (tag=%s)", candidate_id,
             restore_state, verdict_tag ? verdict_tag : "");
   return 0;
}

int learning_judge_commit(const char *candidate_id, const char *candidate_kind,
                          int min_corroboration)
{
   if (!candidate_id || !candidate_id[0])
      return -1;
   const char *surface = learning_candidate_target_surface(candidate_kind);
   if (!surface)
      return -1;

   int corroboration = db2_artifact_citation_count(candidate_id);
   if (corroboration < 0)
      return -1;
   if (corroboration < (min_corroboration > 0 ? min_corroboration : 1))
      return 0; /* not enough corroborating evidence — stays proposed */

   if (db2_artifact_set_state(candidate_id, "committed") != 0)
      return -1;

   char audit_id[37];
   db2_artifact_gen_id(audit_id, sizeof(audit_id));
   char after_json[160];
   snprintf(after_json, sizeof(after_json), "{\"state\":\"committed\",\"corroboration\":%d}",
            corroboration);
   if (db2_audit_event_write(audit_id, candidate_id, surface, "", "kb.learning.judge", "", "", 1.0,
                             0, "{\"state\":\"proposed\"}", after_json) != 0)
      return -1;

   aimee_log(LOG_DEBUG, "learning_evidence",
             "judge committed %s (kind=%s) -> %s (corroboration=%d)", candidate_id, candidate_kind,
             surface, corroboration);
   return 1;
}

int learning_evidence_write_feedback(const char *polarity, const char *title,
                                     const char *description, const char *operator_id,
                                     char *artifact_id_out, int artifact_id_out_len)
{
   if (artifact_id_out && artifact_id_out_len > 0)
      artifact_id_out[0] = '\0';

   if (!polarity || !title)
      return -1;

   const char *kind =
       (strcmp(polarity, "positive") == 0) ? "feedback_positive" : "feedback_negative";

   char id[64];
   db2_artifact_gen_id(id, sizeof(id));

   char payload[512];
   build_feedback_payload(polarity, title, description, payload, sizeof(payload));

   int rc = db2_artifact_write(id, kind, "proposed", "user", operator_id ? operator_id : "",
                               operator_id ? operator_id : "", 1.0, payload);
   if (rc != 0)
   {
      aimee_log(LOG_DEBUG, "learning_evidence", "feedback artifact write failed (kind=%s)", kind);
      return -1;
   }

   if (artifact_id_out && artifact_id_out_len > 0)
      snprintf(artifact_id_out, (size_t)artifact_id_out_len, "%s", id);

   aimee_log(LOG_DEBUG, "learning_evidence", "feedback evidence artifact: id=%s kind=%s", id, kind);
   return 0;
}

int learning_evidence_write_retrieval_event(const char *query_fingerprint, const char *role,
                                            const int64_t *surfaced_ids, int n_surfaced,
                                            char *id_out, int id_out_len)
{
   if (id_out && id_out_len > 0)
      id_out[0] = '\0';

   int rc = db2_demotion_retrieval_event_write(query_fingerprint, role, surfaced_ids, n_surfaced,
                                               id_out, id_out_len);
   if (rc != 0)
   {
      aimee_log(LOG_DEBUG, "learning_evidence", "retrieval_event write failed");
      return -1;
   }
   return 0;
}

int learning_evidence_write_retrieval_attribution(const char *retrieval_event_id,
                                                  int64_t surfaced_row_id, const char *verdict,
                                                  double weight)
{
   if (!retrieval_event_id || !retrieval_event_id[0])
      return -1;

   int rc = db2_demotion_retrieval_attribution_write(retrieval_event_id, surfaced_row_id, verdict,
                                                     weight);
   if (rc != 0)
   {
      aimee_log(LOG_DEBUG, "learning_evidence", "retrieval_attribution write failed (row=%lld)",
                (long long)surfaced_row_id);
      return -1;
   }
   return 0;
}
