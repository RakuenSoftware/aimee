/* db2/artifacts.c: charter artifact table — DB2 (Postgres via libpq).
 * See docs/proposals/done/cross-source-learning-substrate.md */

#include "artifacts.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "feature_rows.h"
#include "kb_mdl.h"
#include "platform_random.h"
#include "aimee.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARTIFACT_MDL_FEATURE_SET_VERSION "mdl-v1"

void db2_artifact_gen_id(char *buf, size_t len)
{
   unsigned char raw[16];
   if (platform_random_bytes(raw, sizeof(raw)) != 0)
      memset(raw, 0, sizeof(raw));
   snprintf(buf, len,
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x"
            "-%02x%02x%02x%02x%02x%02x",
            raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10],
            raw[11], raw[12], raw[13], raw[14], raw[15]);
}

static const char *artifact_payload_string(cJSON *root, const char *name)
{
   cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
   return cJSON_IsString(item) ? item->valuestring : NULL;
}

static const char *artifact_payload_candidate(cJSON *root)
{
   const char *v = artifact_payload_string(root, "candidate");
   if (v && v[0])
      return v;
   v = artifact_payload_string(root, "content");
   if (v && v[0])
      return v;
   v = artifact_payload_string(root, "summary");
   if (v && v[0])
      return v;
   v = artifact_payload_string(root, "narrative");
   if (v && v[0])
      return v;
   return artifact_payload_string(root, "text");
}

static const char *artifact_payload_evidence(cJSON *root)
{
   const char *v = artifact_payload_string(root, "evidence_bundle");
   if (v && v[0])
      return v;
   v = artifact_payload_string(root, "evidence");
   if (v && v[0])
      return v;
   v = artifact_payload_string(root, "source_bundle");
   if (v && v[0])
      return v;
   return artifact_payload_string(root, "sources_text");
}

static int artifact_payload_rank(cJSON *root)
{
   cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "mdl_rank_in_cluster");
   if (!cJSON_IsNumber(item))
      item = cJSON_GetObjectItemCaseSensitive(root, "rank_in_cluster");
   return cJSON_IsNumber(item) && item->valueint > 0 ? item->valueint : 1;
}

static void db2_artifact_emit_mdl_features_if_needed(const char *id, const char *new_state)
{
   if (!id || !id[0] || !new_state || strcmp(new_state, "committed") != 0)
      return;

   void *conn = db2_conn();
   if (!conn)
      return;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT kind, payload, scope_kind, scope_id FROM artifacts WHERE id = ?1", err,
       sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_text(st, "?1", id);
   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      return;
   }

   const char *kind = aimee_pg_column_text(st, 0);
   const char *payload = aimee_pg_column_text(st, 1);
   const char *scope_kind = aimee_pg_column_text(st, 2);
   const char *scope_id = aimee_pg_column_text(st, 3);
   if (!kind || strcmp(kind, "synthesis") != 0 || !payload || !payload[0])
   {
      aimee_pg_finalize(st);
      return;
   }

   char *payload_copy = strdup(payload);
   if (!payload_copy)
   {
      aimee_pg_finalize(st);
      return;
   }
   char scope_kind_copy[64];
   char scope_id_copy[128];
   snprintf(scope_kind_copy, sizeof(scope_kind_copy), "%s", scope_kind ? scope_kind : "");
   snprintf(scope_id_copy, sizeof(scope_id_copy), "%s", scope_id ? scope_id : "");
   aimee_pg_finalize(st);

   cJSON *root = cJSON_Parse(payload_copy);
   if (!root)
   {
      free(payload_copy);
      return;
   }
   const char *candidate = artifact_payload_candidate(root);
   const char *evidence = artifact_payload_evidence(root);
   if (!candidate || !candidate[0])
      candidate = payload_copy;
   if (!evidence || !evidence[0])
      evidence = candidate;

   kb_mdl_score_t score;
   if (kb_mdl_score(candidate, evidence, &score) == 0)
   {
      int rank = artifact_payload_rank(root);
      char features[256];
      snprintf(features, sizeof(features),
               "{\"mdl.l_candidate\":%.6f,\"mdl.l_residual\":%.6f,"
               "\"mdl.total\":%.6f,\"mdl.rank_in_cluster\":%d}",
               score.l_candidate, score.l_residual, score.total, rank);
      (void)db2_feature_row_upsert(id, "artifact", scope_kind_copy, scope_id_copy,
                                   ARTIFACT_MDL_FEATURE_SET_VERSION, features, NULL);
   }
   cJSON_Delete(root);
   free(payload_copy);
}

int db2_artifact_write(const char *id, const char *kind, const char *state, const char *scope_kind,
                       const char *scope_id, const char *operator_id, double confidence,
                       const char *payload_json)
{
   void *conn = db2_conn();
   if (!conn || !id || !kind)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "INSERT INTO artifacts"
                                          " (id, kind, state, scope_kind, scope_id, operator_id,"
                                          "  confidence, attempt_count, source_bundle_hash,"
                                          "  model_version, prompt_version, target_surface,"
                                          "  created_at, payload)"
                                          " VALUES (?1,?2,?3,?4,?5,?6,?7,1,'','','','',?8,?9)"
                                          " ON CONFLICT (id) DO NOTHING",
                                          err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", id);
   aimee_pg_bind_text(st, "?2", kind);
   aimee_pg_bind_text(st, "?3", state ? state : "proposed");
   aimee_pg_bind_text(st, "?4", scope_kind ? scope_kind : "user");
   aimee_pg_bind_text(st, "?5", scope_id ? scope_id : "");
   aimee_pg_bind_text(st, "?6", operator_id ? operator_id : "");
   aimee_pg_bind_double(st, "?7", confidence);
   aimee_pg_bind_text(st, "?8", ts);
   aimee_pg_bind_text(st, "?9", payload_json ? payload_json : "{}");

   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   if (rc != AIMEE_PG_DONE && rc != AIMEE_PG_ROW)
      return -1;
   db2_artifact_emit_mdl_features_if_needed(id, state ? state : "proposed");
   return 0;
}

int db2_artifact_write_ex(const char *id, const char *kind, const char *state,
                          const char *scope_kind, const char *scope_id, const char *operator_id,
                          double confidence, int attempt_count, const char *payload_json)
{
   void *conn = db2_conn();
   if (!conn || !id || !kind)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "INSERT INTO artifacts"
                                          " (id, kind, state, scope_kind, scope_id, operator_id,"
                                          "  confidence, attempt_count, source_bundle_hash,"
                                          "  model_version, prompt_version, target_surface,"
                                          "  created_at, payload)"
                                          " VALUES (?1,?2,?3,?4,?5,?6,?7,?8,'','','','',?9,?10)"
                                          " ON CONFLICT (id) DO NOTHING",
                                          err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", id);
   aimee_pg_bind_text(st, "?2", kind);
   aimee_pg_bind_text(st, "?3", state ? state : "proposed");
   aimee_pg_bind_text(st, "?4", scope_kind ? scope_kind : "user");
   aimee_pg_bind_text(st, "?5", scope_id ? scope_id : "");
   aimee_pg_bind_text(st, "?6", operator_id ? operator_id : "");
   aimee_pg_bind_double(st, "?7", confidence);
   aimee_pg_bind_int(st, "?8", attempt_count > 0 ? attempt_count : 1);
   aimee_pg_bind_text(st, "?9", ts);
   aimee_pg_bind_text(st, "?10", payload_json ? payload_json : "{}");

   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_artifact_write_evidence(const char *kind, const char *scope_kind, const char *scope_id,
                                const char *operator_id, const char *content_hash,
                                const char *payload_json, char *id_out, int id_out_len)
{
   if (id_out && id_out_len > 0)
      id_out[0] = '\0';
   void *conn = db2_conn();
   if (!conn || !kind || !kind[0])
      return -1;

   /* Idempotent capture: identical evidence (same kind + content_hash) collapses
    * to the existing row, so re-uploading a session/feedback/tool batch never
    * creates duplicate evidence. content_hash is stored in source_bundle_hash. */
   char err[256] = "";
   if (content_hash && content_hash[0])
   {
      aimee_pg_stmt_t *q = aimee_pg_prepare(
          conn, "SELECT id FROM artifacts WHERE kind = ?1 AND source_bundle_hash = ?2 LIMIT 1", err,
          sizeof(err));
      if (q)
      {
         aimee_pg_bind_text(q, "?1", kind);
         aimee_pg_bind_text(q, "?2", content_hash);
         if (aimee_pg_step(q, err, sizeof(err)) == AIMEE_PG_ROW)
         {
            const char *existing = aimee_pg_column_text(q, 0);
            if (id_out && id_out_len > 0 && existing)
               snprintf(id_out, (size_t)id_out_len, "%s", existing);
            aimee_pg_finalize(q);
            return 0;
         }
         aimee_pg_finalize(q);
      }
   }

   char id[37];
   db2_artifact_gen_id(id, sizeof(id));
   char ts[32];
   now_utc(ts, sizeof(ts));

   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO artifacts"
                        " (id, kind, state, scope_kind, scope_id, operator_id,"
                        "  confidence, attempt_count, source_bundle_hash,"
                        "  model_version, prompt_version, target_surface,"
                        "  created_at, payload)"
                        " VALUES (?1,?2,'proposed',?3,?4,?5,1.0,1,?6,'','','',?7,?8)"
                        " ON CONFLICT (id) DO NOTHING",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", id);
   aimee_pg_bind_text(st, "?2", kind);
   aimee_pg_bind_text(st, "?3", scope_kind ? scope_kind : "user");
   aimee_pg_bind_text(st, "?4", scope_id ? scope_id : "");
   aimee_pg_bind_text(st, "?5", operator_id ? operator_id : "");
   aimee_pg_bind_text(st, "?6", content_hash ? content_hash : "");
   aimee_pg_bind_text(st, "?7", ts);
   aimee_pg_bind_text(st, "?8", payload_json ? payload_json : "{}");

   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   if (rc != AIMEE_PG_DONE && rc != AIMEE_PG_ROW)
      return -1;
   if (id_out && id_out_len > 0)
      snprintf(id_out, (size_t)id_out_len, "%s", id);
   return 0;
}

/* Read the before_snapshot of the most recent audit event for an artifact.
 * Writes the JSON text into out (or "" if none). Returns 0 on success (even
 * when there is no audit row), -1 on error. */
int db2_audit_read_latest_before(const char *artifact_id, char *out, int out_len)
{
   if (out && out_len > 0)
      out[0] = '\0';
   if (!artifact_id || !artifact_id[0] || !out || out_len <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT before_snapshot FROM audit_events"
                                          " WHERE source_artifact_id = ?1"
                                          " ORDER BY id DESC LIMIT 1",
                                          err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", artifact_id);
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *v = aimee_pg_column_text(st, 0);
      if (v)
         snprintf(out, (size_t)out_len, "%s", v);
   }
   aimee_pg_finalize(st);
   return 0;
}

int db2_artifact_review_rollback(const char *artifact_id, const char *restore_state,
                                 const char *verdict_tag, const char *verdict_scope,
                                 const char *counter_example)
{
   if (!artifact_id || !artifact_id[0] || !restore_state || !restore_state[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* Capture the current state for the audit before/after, then roll the
    * artifact back to restore_state (its pre-commit snapshot state). */
   db2_artifact_row_t cur;
   char before_json[64] = "{\"state\":\"committed\"}";
   if (db2_artifact_read(artifact_id, &cur, NULL, 0, NULL) == 0 && cur.state[0])
      snprintf(before_json, sizeof(before_json), "{\"state\":\"%s\"}", cur.state);

   if (db2_artifact_set_state(artifact_id, restore_state) != 0)
      return -1;

   char after_json[64];
   snprintf(after_json, sizeof(after_json), "{\"state\":\"%s\"}", restore_state);

   char audit_id[37];
   db2_artifact_gen_id(audit_id, sizeof(audit_id));

   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO audit_events"
                        " (id, source_artifact_id, target_surface, target_id, operator_id,"
                        "  scope_kind, scope_id, before_snapshot, after_snapshot,"
                        "  verdict, verdict_tag, verdict_scope, counter_example)"
                        " VALUES (?1,?2,'','','kb.learning.review','','',?3,?4,"
                        "'thumbs_down',?5,?6,?7)",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", audit_id);
   aimee_pg_bind_text(st, "?2", artifact_id);
   aimee_pg_bind_text(st, "?3", before_json);
   aimee_pg_bind_text(st, "?4", after_json);
   aimee_pg_bind_text(st, "?5", verdict_tag ? verdict_tag : "");
   aimee_pg_bind_text(st, "?6", verdict_scope ? verdict_scope : "");
   aimee_pg_bind_text(st, "?7", counter_example ? counter_example : "");
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_artifact_verdict_suppressed(const char *verdict_tag, const char *verdict_scope)
{
   if (!verdict_tag || !verdict_tag[0])
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT COUNT(*) FROM audit_events"
                                          " WHERE verdict = 'thumbs_down' AND verdict_tag = ?1"
                                          "   AND verdict_scope = ?2",
                                          err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", verdict_tag);
   aimee_pg_bind_text(st, "?2", verdict_scope ? verdict_scope : "");
   int suppressed = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      suppressed = aimee_pg_column_int(st, 0) > 0;
   aimee_pg_finalize(st);
   return suppressed;
}

int db2_artifact_citation_count(const char *artifact_id)
{
   if (!artifact_id || !artifact_id[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT COUNT(DISTINCT source_id) FROM artifact_citations WHERE artifact_id = ?1", err,
       sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", artifact_id);
   int n = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      n = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return n;
}

int db2_artifact_set_state(const char *id, const char *new_state)
{
   void *conn = db2_conn();
   if (!conn || !id || !new_state)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "UPDATE artifacts SET state = ?1 WHERE id = ?2", err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", new_state);
   aimee_pg_bind_text(st, "?2", id);

   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   if (rc != AIMEE_PG_DONE && rc != AIMEE_PG_ROW)
      return -1;
   db2_artifact_emit_mdl_features_if_needed(id, new_state);
   return 0;
}

int db2_artifact_set_target_surface(const char *id, const char *target_surface)
{
   void *conn = db2_conn();
   if (!conn || !id || !target_surface)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "UPDATE artifacts SET target_surface = ?1 WHERE id = ?2", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", target_surface);
   aimee_pg_bind_text(st, "?2", id);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_artifact_target_surface(const char *id, char *out, int out_len)
{
   if (out && out_len > 0)
      out[0] = '\0';
   if (!id || !id[0] || !out || out_len <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT target_surface FROM artifacts WHERE id = ?1 LIMIT 1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", id);
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *v = aimee_pg_column_text(st, 0);
      if (v)
         snprintf(out, (size_t)out_len, "%s", v);
   }
   aimee_pg_finalize(st);
   return 0;
}

int db2_artifact_register_exemplar(const char *artifact_id, const char *collection)
{
   void *conn = db2_conn();
   if (!conn || !artifact_id || !artifact_id[0])
      return -1;
   /* Register the artifact as a case/guardrail exemplar. The embedding column
    * is left at its default (filled asynchronously by the embed pipeline), so
    * promotion does not block on the embedder. */
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "INSERT INTO exemplar_vectors (artifact_id, collection) VALUES (?1, ?2)", err,
       sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", artifact_id);
   aimee_pg_bind_text(st, "?2", collection && collection[0] ? collection : "case_exemplars");
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_artifact_touch(const char *id)
{
   void *conn = db2_conn();
   if (!conn || !id || !id[0])
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "UPDATE artifacts SET last_accessed_at = CURRENT_TIMESTAMP WHERE id = ?1", err,
       sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", id);

   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   int changed = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE && changed > 0) ? 0 : -1;
}

int db2_artifact_cite(const char *artifact_id, const char *source_kind, const char *source_id)
{
   void *conn = db2_conn();
   if (!conn || !artifact_id || !source_kind || !source_id)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO artifact_citations"
                        " (artifact_id, source_kind, source_id, span_start, span_end)"
                        " VALUES (?1,?2,?3,0,0)"
                        " ON CONFLICT DO NOTHING",
                        err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", artifact_id);
   aimee_pg_bind_text(st, "?2", source_kind);
   aimee_pg_bind_text(st, "?3", source_id);

   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_artifact_link(const char *from_id, const char *to_id, const char *link_kind)
{
   void *conn = db2_conn();
   if (!conn || !from_id || !to_id || !link_kind)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "INSERT INTO artifact_links (from_id, to_id, kind)"
                                          " VALUES (?1,?2,?3)"
                                          " ON CONFLICT DO NOTHING",
                                          err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", from_id);
   aimee_pg_bind_text(st, "?2", to_id);
   aimee_pg_bind_text(st, "?3", link_kind);

   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_audit_event_write(const char *id, const char *source_artifact_id,
                          const char *target_surface, const char *target_id,
                          const char *operator_id, const char *scope_kind, const char *scope_id,
                          double applied_confidence, int flagged_for_review,
                          const char *before_json, const char *after_json)
{
   void *conn = db2_conn();
   if (!conn || !id || !source_artifact_id || !target_surface || !target_id)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "INSERT INTO audit_events"
                                          " (id, source_artifact_id, target_surface, target_id,"
                                          "  operator_id, scope_kind, scope_id,"
                                          "  applied_at, applied_confidence, flagged_for_review,"
                                          "  before_snapshot, after_snapshot)"
                                          " VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)"
                                          " ON CONFLICT (id) DO NOTHING",
                                          err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", id);
   aimee_pg_bind_text(st, "?2", source_artifact_id);
   aimee_pg_bind_text(st, "?3", target_surface);
   aimee_pg_bind_text(st, "?4", target_id);
   aimee_pg_bind_text(st, "?5", operator_id ? operator_id : "");
   aimee_pg_bind_text(st, "?6", scope_kind ? scope_kind : "user");
   aimee_pg_bind_text(st, "?7", scope_id ? scope_id : "");
   aimee_pg_bind_text(st, "?8", ts);
   aimee_pg_bind_double(st, "?9", applied_confidence);
   aimee_pg_bind_int(st, "?10", flagged_for_review ? 1 : 0);
   if (before_json)
      aimee_pg_bind_text(st, "?11", before_json);
   else
      aimee_pg_bind_null(st, "?11");
   if (after_json)
      aimee_pg_bind_text(st, "?12", after_json);
   else
      aimee_pg_bind_null(st, "?12");

   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_audit_event_list(const char *since, const char *until, const char *scope_kind, int limit,
                         db2_audit_event_row_t *out, int max)
{
   if (!out || max <= 0 || !since || !since[0])
      return -1; /* since is required: no unbounded full-table scans */
   void *conn = db2_conn();
   if (!conn)
      return -1;
   if (limit <= 0 || limit > max)
      limit = max;
   char sql[512];
   int pos = snprintf(sql, sizeof(sql),
                      "SELECT id, target_surface, target_id, operator_id, scope_kind, scope_id,"
                      " applied_at, applied_confidence, flagged_for_review"
                      " FROM audit_events WHERE applied_at >= ?1");
   int has_until = until && until[0];
   int has_scope = scope_kind && scope_kind[0];
   if (has_until)
      pos += snprintf(sql + pos, sizeof(sql) - (size_t)pos, " AND applied_at <= ?2");
   if (has_scope)
      pos += snprintf(sql + pos, sizeof(sql) - (size_t)pos, " AND scope_kind = ?3");
   snprintf(sql + pos, sizeof(sql) - (size_t)pos, " ORDER BY applied_at DESC LIMIT ?4");

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", since);
   if (has_until)
      aimee_pg_bind_text(st, "?2", until);
   if (has_scope)
      aimee_pg_bind_text(st, "?3", scope_kind);
   aimee_pg_bind_int64(st, "?4", limit);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_audit_event_row_t *r = &out[n++];
      memset(r, 0, sizeof(*r));
      const char *c;
      c = aimee_pg_column_text(st, 0);
      snprintf(r->id, sizeof(r->id), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 1);
      snprintf(r->target_surface, sizeof(r->target_surface), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 2);
      snprintf(r->target_id, sizeof(r->target_id), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 3);
      snprintf(r->operator_id, sizeof(r->operator_id), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 4);
      snprintf(r->scope_kind, sizeof(r->scope_kind), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 5);
      snprintf(r->scope_id, sizeof(r->scope_id), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 6);
      snprintf(r->applied_at, sizeof(r->applied_at), "%s", c ? c : "");
      r->applied_confidence = aimee_pg_column_double(st, 7);
      r->flagged_for_review = (int)aimee_pg_column_int64(st, 8);
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_artifact_count(const char *kind, const char *state)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char sql[256];
   char err[256] = "";

   if (kind && state)
      snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM artifacts WHERE kind = ?1 AND state = ?2");
   else if (kind)
      snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM artifacts WHERE kind = ?1");
   else if (state)
      snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM artifacts WHERE state = ?1");
   else
      snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM artifacts");

   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;

   if (kind && state)
   {
      aimee_pg_bind_text(st, "?1", kind);
      aimee_pg_bind_text(st, "?2", state);
   }
   else if (kind)
      aimee_pg_bind_text(st, "?1", kind);
   else if (state)
      aimee_pg_bind_text(st, "?1", state);

   int count = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      count = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return count;
}

int db2_artifact_list_proposed(const char *target_surface, int limit, db2_artifact_proposed_t *out,
                               int max_out)
{
   void *conn = db2_conn();
   if (!conn || !out || max_out <= 0)
      return -1;

   const char *sql_surf =
       "SELECT a.id, a.kind, a.target_surface, a.confidence, a.created_at, a.payload,"
       "       string_agg(c.source_id, ',') AS citation_ids"
       " FROM artifacts a"
       " LEFT JOIN artifact_citations c ON c.artifact_id = a.id"
       " WHERE a.state = 'proposed' AND a.target_surface = ?1"
       " GROUP BY a.id, a.kind, a.target_surface, a.confidence, a.created_at, a.payload"
       " ORDER BY a.confidence DESC"
       " LIMIT ?2";

   const char *sql_all =
       "SELECT a.id, a.kind, a.target_surface, a.confidence, a.created_at, a.payload,"
       "       string_agg(c.source_id, ',') AS citation_ids"
       " FROM artifacts a"
       " LEFT JOIN artifact_citations c ON c.artifact_id = a.id"
       " WHERE a.state = 'proposed'"
       " GROUP BY a.id, a.kind, a.target_surface, a.confidence, a.created_at, a.payload"
       " ORDER BY a.confidence DESC"
       " LIMIT ?1";

   char err[256] = "";
   aimee_pg_stmt_t *st;

   if (target_surface && target_surface[0])
   {
      st = aimee_pg_prepare(conn, sql_surf, err, sizeof(err));
      if (!st)
         return -1;
      aimee_pg_bind_text(st, "?1", target_surface);
      aimee_pg_bind_int(st, "?2", limit > 0 ? limit : 50);
   }
   else
   {
      st = aimee_pg_prepare(conn, sql_all, err, sizeof(err));
      if (!st)
         return -1;
      aimee_pg_bind_int(st, "?1", limit > 0 ? limit : 50);
   }

   int n = 0;
   while (n < max_out && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_artifact_proposed_t *row = &out[n++];
      memset(row, 0, sizeof(*row));
      const char *v;
      v = aimee_pg_column_text(st, 0);
      if (v)
         snprintf(row->id, sizeof(row->id), "%s", v);
      v = aimee_pg_column_text(st, 1);
      if (v)
         snprintf(row->kind, sizeof(row->kind), "%s", v);
      v = aimee_pg_column_text(st, 2);
      if (v)
         snprintf(row->target_surface, sizeof(row->target_surface), "%s", v);
      row->confidence = aimee_pg_column_double(st, 3);
      v = aimee_pg_column_text(st, 4);
      if (v)
         snprintf(row->created_at, sizeof(row->created_at), "%s", v);
      v = aimee_pg_column_text(st, 5);
      if (v)
         snprintf(row->payload_json, sizeof(row->payload_json), "%s", v);
      v = aimee_pg_column_text(st, 6);
      if (v)
         snprintf(row->citation_ids, sizeof(row->citation_ids), "%s", v);
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_artifact_reject(const char *id, const char *verdict_tag, const char *verdict_scope,
                        const char *counter_example, const char *before_json)
{
   if (db2_artifact_set_state(id, "rejected") != 0)
      return -1;

   char audit_id[37];
   db2_artifact_gen_id(audit_id, sizeof(audit_id));

   cJSON *after = cJSON_CreateObject();
   cJSON_AddStringToObject(after, "state", "rejected");
   if (verdict_tag && verdict_tag[0])
      cJSON_AddStringToObject(after, "verdict_tag", verdict_tag);
   if (verdict_scope && verdict_scope[0])
      cJSON_AddStringToObject(after, "verdict_scope", verdict_scope);
   if (counter_example && counter_example[0])
      cJSON_AddStringToObject(after, "counter_example", counter_example);
   char *after_json = cJSON_PrintUnformatted(after);
   cJSON_Delete(after);

   int rc = db2_audit_event_write(audit_id, id, "", "", "", "user", "", 0.0, 0,
                                  before_json ? before_json : "{}", after_json ? after_json : "{}");
   free(after_json);
   return rc;
}

/* Does the payload's "components" array contain |component| (case-insensitive)? */
static int payload_has_component(const cJSON *payload, const char *component)
{
   const cJSON *comps = cJSON_GetObjectItemCaseSensitive(payload, "components");
   if (!cJSON_IsArray(comps))
      return 0;
   const cJSON *c = NULL;
   cJSON_ArrayForEach(c, comps)
   {
      if (cJSON_IsString(c) && strcasecmp(c->valuestring, component) == 0)
         return 1;
   }
   return 0;
}

/* Does the payload's string field |key| equal |want| (case-insensitive)? */
static int payload_field_eq(const cJSON *payload, const char *key, const char *want)
{
   const cJSON *j = cJSON_GetObjectItemCaseSensitive(payload, key);
   return cJSON_IsString(j) && strcasecmp(j->valuestring, want) == 0;
}

int db2_artifact_filter_facets(int64_t release_id, const char *kind, const char *status,
                               const char *priority, const char *component, db2_artifact_row_t *out,
                               int max)
{
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* `kind`, live-state, and (optionally) release binding are filtered in SQL
    * (portable). The narrative-payload facets (status / priority / component)
    * are matched in C against the parsed JSON payload — the charter keeps
    * facets in the payload, not a side table, and matching here avoids
    * non-portable JSON SQL (postgres ->>'x' vs sqlite json_extract). Candidates
    * are capped; the result is precision-exact: every returned row satisfies
    * every supplied facet (and, when release_id > 0, is cited by a kb_document
    * in that release). */
   char sql[1024];
   int p = snprintf(sql, sizeof(sql),
                    "SELECT id, kind, state, scope_kind, scope_id, confidence, payload,"
                    " COALESCE(NULLIF(committed_at, ''), created_at)"
                    " FROM artifacts WHERE state IN ('proposed','committed')");
   if (kind && kind[0])
      p += snprintf(sql + p, sizeof(sql) - (size_t)p, " AND kind = ?1");
   if (release_id > 0)
      /* release_id is a trusted int64 (never user text); inline it so the
       * optional kind ?1 binding keeps a stable parameter index. */
      p += snprintf(sql + p, sizeof(sql) - (size_t)p,
                    " AND id IN (SELECT c.artifact_id FROM artifact_citations c"
                    " JOIN release_docs rd ON CAST(rd.doc_id AS TEXT) = c.source_id"
                    " WHERE c.source_kind = 'kb_document' AND rd.release_id = %lld)",
                    (long long)release_id);
   p += snprintf(sql + p, sizeof(sql) - (size_t)p, " ORDER BY id LIMIT 2000");

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   if (kind && kind[0])
      aimee_pg_bind_text(st, "?1", kind);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *payload_text = aimee_pg_column_text(st, 6);
      cJSON *payload = payload_text ? cJSON_Parse(payload_text) : NULL;

      int ok = 1;
      if (status && status[0] && !payload_field_eq(payload, "status", status))
         ok = 0;
      if (ok && priority && priority[0] && !payload_field_eq(payload, "priority", priority))
         ok = 0;
      if (ok && component && component[0] && !payload_has_component(payload, component))
         ok = 0;

      if (ok)
      {
         db2_artifact_row_t *r = &out[n];
         memset(r, 0, sizeof(*r));
         const char *v;
         if ((v = aimee_pg_column_text(st, 0)))
            snprintf(r->id, sizeof(r->id), "%s", v);
         if ((v = aimee_pg_column_text(st, 1)))
            snprintf(r->kind, sizeof(r->kind), "%s", v);
         if ((v = aimee_pg_column_text(st, 2)))
            snprintf(r->state, sizeof(r->state), "%s", v);
         if ((v = aimee_pg_column_text(st, 3)))
            snprintf(r->scope_kind, sizeof(r->scope_kind), "%s", v);
         if ((v = aimee_pg_column_text(st, 4)))
            snprintf(r->scope_id, sizeof(r->scope_id), "%s", v);
         r->confidence = aimee_pg_column_double(st, 5);
         if (payload_text)
            snprintf(r->payload_json, sizeof(r->payload_json), "%s", payload_text);
         if ((v = aimee_pg_column_text(st, 7)))
            snprintf(r->updated_at, sizeof(r->updated_at), "%s", v);
         n++;
      }

      cJSON_Delete(payload);
   }
   aimee_pg_finalize(st);
   return n;
}

/* Version-bump (model_version): re-embed every committed curator artifact by
 * sending it back to 'proposed' so the index passes re-embed it WITHOUT
 * re-extracting. Returns the number of artifacts re-queued. */
int db2_curator_reembed_all(void)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "UPDATE artifacts SET state = 'proposed'"
       " WHERE state = 'committed'"
       "   AND kind IN ('doc_summary','synthesis','open_question','claim','entity','code_unit')",
       err, sizeof(err));
   if (!st)
      return 0;
   (void)aimee_pg_step(st, err, sizeof(err));
   int n = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return n;
}

int db2_artifact_invalidate_citing(const char *source_kind, const char *source_id,
                                   int edit_span_start, int edit_span_end)
{
   void *conn = db2_conn();
   if (!conn || !source_kind || !source_id)
      return -1;

   /* A citation matches when it cites the whole source (span 0,0), when the
    * edit is whole-source (edit span 0,0), or when its [span_start,span_end]
    * overlaps the edited [edit_span_start,edit_span_end). Only artifacts still
    * live (proposed / committed) are invalidated; already retired/rejected/
    * stale rows are left alone. Drained in batches so a single source with many
    * citing artifacts doesn't need an unbounded id buffer. */
   static const char *sel = "SELECT DISTINCT a.id, a.state"
                            " FROM artifacts a"
                            " JOIN artifact_citations c ON c.artifact_id = a.id"
                            " WHERE c.source_kind = ?1 AND c.source_id = ?2"
                            "   AND a.state IN ('proposed','committed')"
                            "   AND ( (c.span_start = 0 AND c.span_end = 0)"
                            "      OR (?3 = 0 AND ?4 = 0)"
                            "      OR (c.span_start < ?4 AND c.span_end > ?3) )"
                            " LIMIT 256";

   int total = 0;
   for (;;)
   {
      char err[256] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sel, err, sizeof(err));
      if (!st)
         return -1;
      aimee_pg_bind_text(st, "?1", source_kind);
      aimee_pg_bind_text(st, "?2", source_id);
      aimee_pg_bind_int(st, "?3", edit_span_start);
      aimee_pg_bind_int(st, "?4", edit_span_end);

      char ids[256][37];
      char states[256][32];
      int n = 0;
      while (n < 256 && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         const char *id = aimee_pg_column_text(st, 0);
         const char *state = aimee_pg_column_text(st, 1);
         snprintf(ids[n], sizeof(ids[n]), "%s", id ? id : "");
         snprintf(states[n], sizeof(states[n]), "%s", state ? state : "");
         if (ids[n][0])
            n++;
      }
      aimee_pg_finalize(st);
      if (n == 0)
         break;

      for (int i = 0; i < n; i++)
      {
         if (db2_artifact_set_state(ids[i], "stale") != 0)
            return -1;

         char audit_id[37];
         db2_artifact_gen_id(audit_id, sizeof(audit_id));

         char before_json[128];
         snprintf(before_json, sizeof(before_json), "{\"state\":\"%s\"}", states[i]);

         cJSON *after = cJSON_CreateObject();
         cJSON_AddStringToObject(after, "state", "stale");
         cJSON_AddStringToObject(after, "reason", "cited_source_changed");
         char src[320];
         snprintf(src, sizeof(src), "%s:%s", source_kind, source_id);
         cJSON_AddStringToObject(after, "source", src);
         char *after_json = cJSON_PrintUnformatted(after);
         cJSON_Delete(after);

         int rc = db2_audit_event_write(audit_id, ids[i], "", "", "kb.curator.invalidate", "", "",
                                        0.0, 0, before_json, after_json ? after_json : "{}");
         free(after_json);
         if (rc != 0)
            return -1;
         total++;
      }
   }

   return total;
}

int db2_artifact_stamp_reflected(const char *id)
{
   void *conn = db2_conn();
   if (!conn || !id || !id[0])
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "UPDATE artifacts SET reflected_at = CURRENT_TIMESTAMP WHERE id = ?1",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", id);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_artifact_read(const char *id, db2_artifact_row_t *out, db2_artifact_citation_t *citations,
                      int max_citations, int *citation_count)
{
   if (!id || !id[0] || !out)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT id, kind, state, scope_kind, scope_id, confidence, payload,"
                        " COALESCE(NULLIF(committed_at, ''), created_at)"
                        " FROM artifacts WHERE id = ?1 LIMIT 1",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", id);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   if (step != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      return -1;
   }
   memset(out, 0, sizeof(*out));
   const char *v;
   v = aimee_pg_column_text(st, 0);
   if (v)
      snprintf(out->id, sizeof(out->id), "%s", v);
   v = aimee_pg_column_text(st, 1);
   if (v)
      snprintf(out->kind, sizeof(out->kind), "%s", v);
   v = aimee_pg_column_text(st, 2);
   if (v)
      snprintf(out->state, sizeof(out->state), "%s", v);
   v = aimee_pg_column_text(st, 3);
   if (v)
      snprintf(out->scope_kind, sizeof(out->scope_kind), "%s", v);
   v = aimee_pg_column_text(st, 4);
   if (v)
      snprintf(out->scope_id, sizeof(out->scope_id), "%s", v);
   out->confidence = aimee_pg_column_double(st, 5);
   v = aimee_pg_column_text(st, 6);
   if (v)
      snprintf(out->payload_json, sizeof(out->payload_json), "%s", v);
   v = aimee_pg_column_text(st, 7);
   if (v)
      snprintf(out->updated_at, sizeof(out->updated_at), "%s", v);
   aimee_pg_finalize(st);

   if (citations && max_citations > 0)
   {
      st = aimee_pg_prepare(conn,
                            "SELECT source_kind, source_id FROM artifact_citations"
                            " WHERE artifact_id = ?1",
                            err, sizeof(err));
      int n = 0;
      if (st)
      {
         aimee_pg_bind_text(st, "?1", id);
         while (n < max_citations && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
         {
            memset(&citations[n], 0, sizeof(citations[n]));
            v = aimee_pg_column_text(st, 0);
            if (v)
               snprintf(citations[n].source_kind, sizeof(citations[n].source_kind), "%s", v);
            v = aimee_pg_column_text(st, 1);
            if (v)
               snprintf(citations[n].source_id, sizeof(citations[n].source_id), "%s", v);
            n++;
         }
         aimee_pg_finalize(st);
      }
      if (citation_count)
         *citation_count = n;
   }
   else if (citation_count)
   {
      *citation_count = 0;
   }
   return 0;
}

int db2_artifact_links_read(const char *id, db2_artifact_link_row_t *out, int max)
{
   if (!id || !id[0])
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT to_id, kind FROM artifact_links WHERE from_id = ?1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", id);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      const char *v = aimee_pg_column_text(st, 0);
      if (v)
         snprintf(out[n].to_id, sizeof(out[n].to_id), "%s", v);
      v = aimee_pg_column_text(st, 1);
      if (v)
         snprintf(out[n].link_kind, sizeof(out[n].link_kind), "%s", v);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_artifact_flag_review(const char *id, const char *reason)
{
   void *conn = db2_conn();
   if (!conn || !id || !id[0])
      return -1;

   const char *r = reason && reason[0] ? reason : "flagged";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "UPDATE artifacts"
       "   SET state = 'proposed',"
       "       payload = CASE"
       "                   WHEN payload IS NULL OR payload = '' THEN"
       "                     json_build_object('flagged_for_review', true,"
       "                                       'flagged_reason', ?2::text)::text"
       "                   ELSE"
       "                     (payload::jsonb ||"
       "                      json_build_object('flagged_for_review', true,"
       "                                        'flagged_reason', ?2::text)::jsonb)::text"
       "                 END"
       " WHERE id = ?1",
       err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", id);
   aimee_pg_bind_text(st, "?2", r);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}
