/* db2/fidelity.c: answer-level fidelity reports + per-chunk attributions
 * (auditable-correctness P3 storage substrate). These are NON-SCORED artifact
 * kinds, structurally invisible to db2_demotion_score (which reads only
 * kind='retrieval_attribution') — fidelity is an answer-level quality signal,
 * never a demotion lever. The LLM entailment judge that produces these rows is a
 * later, default-off increment; this file is storage + read only. */

#include "fidelity.h"
#include "artifacts.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "aimee.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Stamp turn_id onto a freshly-written artifact (mirrors the retrieval_event
 * writer; the INSERT + this UPDATE are not one transaction, which is acceptable
 * for a substrate whose only writer today is unit tests / a default-off judge).
 * The fidelity kinds are not covered by the retrieval_event partial unique index,
 * so they never collide with the turn's retrieval_event. */
static int fidelity_stamp_turn(const char *id, const char *turn_id)
{
   if (!turn_id || !turn_id[0])
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "UPDATE artifacts SET turn_id = ?1 WHERE id = ?2", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", turn_id);
   aimee_pg_bind_text(st, "?2", id);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int db2_fidelity_report_write(const char *turn_id, const char *status, int supported,
                              int unsupported, int abstained)
{
   /* A fidelity report is keyed by turn; a turn-less row could never be read back,
    * so reject it. Validate status against the four exhaustive audit states so a
    * typo can never be stored and silently corrupt the supported/unsupported
    * denominator. */
   if (!turn_id || !turn_id[0])
      return -1;
   if (!status ||
       (strcmp(status, "ok") != 0 && strcmp(status, "not_evaluated") != 0 &&
        strcmp(status, "evidence_unavailable") != 0 && strcmp(status, "not_instrumented") != 0))
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* (Re)judge boundary, DELETE-FIRST: clear this turn's prior report AND its prior
    * per-chunk attributions, so a re-judge fully replaces the turn's fidelity state
    * (no stale attributions left pointing at a superseded report) and exactly one
    * report exists per turn_id (deterministic, no created_at tiebreak). The caller
    * writes fresh attributions AFTER this. Deleting before the INSERT means the
    * just-written row can never be self-removed, regardless of whether
    * db2_artifact_write stamps turn_id. The DELETE + INSERT + stamp are not one
    * transaction — acceptable for this substrate (today the only writer is unit
    * tests / a default-off judge; a crash in the gap merely drops the report, which
    * the turn's re-judge restores). A failed DELETE aborts before inserting so we
    * never leave duplicates. */
   {
      char derr[256] = "";
      aimee_pg_stmt_t *d =
          aimee_pg_prepare(conn,
                           "DELETE FROM artifacts WHERE turn_id = ?1"
                           " AND kind IN ('fidelity_report', 'fidelity_attribution')",
                           derr, sizeof(derr));
      if (!d)
         return -1;
      aimee_pg_bind_text(d, "?1", turn_id);
      int drc = aimee_pg_step(d, derr, sizeof(derr));
      aimee_pg_finalize(d);
      if (drc != AIMEE_PG_DONE)
         return -1;
   }

   cJSON *p = cJSON_CreateObject();
   if (!p)
      return -1;
   cJSON_AddStringToObject(p, "status", status);
   cJSON_AddNumberToObject(p, "supported", supported);
   cJSON_AddNumberToObject(p, "unsupported", unsupported);
   cJSON_AddNumberToObject(p, "abstained", abstained);
   char *payload = cJSON_PrintUnformatted(p);
   cJSON_Delete(p);
   if (!payload)
      return -1;

   char id[64];
   db2_artifact_gen_id(id, sizeof(id));
   /* confidence=1.0 is a fixed sentinel: a fidelity_report is counts+status, not a
    * scored/probabilistic artifact (it is a non-scored kind). operator_id matches
    * the per-chunk attribution ("fidelity-judge") for provenance parity. */
   int rc = db2_artifact_write(id, "fidelity_report", "proposed", "system", "", "fidelity-judge",
                               1.0, payload);
   free(payload);
   if (rc != 0)
      return -1;
   return fidelity_stamp_turn(id, turn_id);
}

int db2_fidelity_report_by_turn(const char *turn_id, char *status_out, int status_out_len,
                                int *supported_out, int *unsupported_out, int *abstained_out)
{
   if (status_out && status_out_len > 0)
      status_out[0] = '\0';
   if (supported_out)
      *supported_out = 0;
   if (unsupported_out)
      *unsupported_out = 0;
   if (abstained_out)
      *abstained_out = 0;
   if (!turn_id || !turn_id[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT payload FROM artifacts"
                                          " WHERE kind = 'fidelity_report' AND turn_id = ?1"
                                          " ORDER BY created_at DESC LIMIT 1",
                                          err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", turn_id);
   int rc = aimee_pg_step(st, err, sizeof(err));
   int result = 0; /* 0 = not found */
   if (rc == AIMEE_PG_ROW)
   {
      const char *payload = aimee_pg_column_text(st, 0);
      cJSON *p = payload ? cJSON_Parse(payload) : NULL;
      if (!p)
      {
         result = -1; /* row exists but payload is malformed — distinct from a
                       * legitimate all-zeros report */
      }
      else
      {
         result = 1;
         cJSON *s = cJSON_GetObjectItemCaseSensitive(p, "status");
         if (status_out && status_out_len > 0 && s && cJSON_IsString(s) && s->valuestring)
            snprintf(status_out, (size_t)status_out_len, "%s", s->valuestring);
         cJSON *sup = cJSON_GetObjectItemCaseSensitive(p, "supported");
         cJSON *uns = cJSON_GetObjectItemCaseSensitive(p, "unsupported");
         cJSON *abst = cJSON_GetObjectItemCaseSensitive(p, "abstained");
         if (supported_out && sup && cJSON_IsNumber(sup))
            *supported_out = sup->valueint;
         if (unsupported_out && uns && cJSON_IsNumber(uns))
            *unsupported_out = uns->valueint;
         if (abstained_out && abst && cJSON_IsNumber(abst))
            *abstained_out = abst->valueint;
         cJSON_Delete(p);
      }
   }
   aimee_pg_finalize(st);
   return result;
}

int db2_fidelity_attribution_write(const char *turn_id, int64_t surfaced_id, const char *verdict)
{
   /* A fidelity row is keyed by turn; a turn-less row could never be read back, so
    * reject it. Validate verdict against the documented enum (parity with the
    * report's status allow-list). */
   if (!turn_id || !turn_id[0])
      return -1;
   if (!verdict || (strcmp(verdict, "accepted") != 0 && strcmp(verdict, "irrelevant") != 0))
      return -1;

   cJSON *p = cJSON_CreateObject();
   if (!p)
      return -1;
   cJSON_AddNumberToObject(p, "surfaced_id", (double)surfaced_id);
   cJSON_AddStringToObject(p, "verdict", verdict);
   char *payload = cJSON_PrintUnformatted(p);
   cJSON_Delete(p);
   if (!payload)
      return -1;

   char id[64];
   db2_artifact_gen_id(id, sizeof(id));
   int rc = db2_artifact_write(id, "fidelity_attribution", "proposed", "system", "",
                               "fidelity-judge", 1.0, payload);
   free(payload);
   if (rc != 0)
      return -1;
   return fidelity_stamp_turn(id, turn_id);
}

int db2_fidelity_attribution_count_by_turn(const char *turn_id)
{
   if (!turn_id || !turn_id[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT COUNT(*) FROM artifacts WHERE kind = 'fidelity_attribution' AND turn_id = ?1",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", turn_id);
   int n = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      n = (int)aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return n;
}
