/* db1/diagnose.c: evidence-driven diagnosis — SQLite-backed implementation. */

#include "diagnose.h"
#include "db1_internal.h"
#include "dstr.h"
#include "cJSON.h"

#include <sqlite3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- DB helpers (private) --- */

static int clamp_rank(int rank)
{
   if (rank < DIAG_RANK_DIRECT)
      return DIAG_RANK_DIRECT;
   if (rank > DIAG_RANK_SPECULATION)
      return DIAG_RANK_SPECULATION;
   return rank;
}

static void touch_diagnosis(sqlite3 *db, int diag_id)
{
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, "UPDATE diagnoses SET updated_at = datetime('now') WHERE id = ?", -1,
                          &stmt, NULL) != SQLITE_OK)
      return;
   sqlite3_bind_int(stmt, 1, diag_id);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
}

static int insert_item(int diag_id, const char *kind, int parent_id, const char *content,
                       const char *source, int rank)
{
   if (!kind || !content || !content[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db,
                          "INSERT INTO diagnosis_items(diagnosis_id, kind, parent_id, content, "
                          "source, evidence_rank)"
                          " VALUES(?, ?, ?, ?, ?, ?)",
                          -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_int(stmt, 1, diag_id);
   sqlite3_bind_text(stmt, 2, kind, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 3, parent_id);
   sqlite3_bind_text(stmt, 4, content, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 5, source ? source : "", -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 6, clamp_rank(rank));
   sqlite3_step(stmt);
   int changes = sqlite3_changes(db);
   int id = (int)sqlite3_last_insert_rowid(db);
   sqlite3_finalize(stmt);

   if (changes > 0)
   {
      touch_diagnosis(db, diag_id);
      return id;
   }
   return -1;
}

static void row_to_diagnosis(sqlite3_stmt *stmt, diagnosis_t *d)
{
   d->id = sqlite3_column_int(stmt, 0);
   db1_copy_col_text(d->symptom, sizeof(d->symptom), stmt, 1);
   db1_copy_col_text(d->status, sizeof(d->status), stmt, 2);
   db1_copy_col_text(d->conclusion, sizeof(d->conclusion), stmt, 3);
   d->confidence = sqlite3_column_double(stmt, 4);
   db1_copy_col_text(d->created_at, sizeof(d->created_at), stmt, 5);
   db1_copy_col_text(d->updated_at, sizeof(d->updated_at), stmt, 6);
}

static void row_to_item(sqlite3_stmt *stmt, diagnosis_item_t *it)
{
   it->id = sqlite3_column_int(stmt, 0);
   it->diagnosis_id = sqlite3_column_int(stmt, 1);
   db1_copy_col_text(it->kind, sizeof(it->kind), stmt, 2);
   it->parent_id = sqlite3_column_int(stmt, 3);
   db1_copy_col_text(it->content, sizeof(it->content), stmt, 4);
   db1_copy_col_text(it->source, sizeof(it->source), stmt, 5);
   it->evidence_rank = sqlite3_column_int(stmt, 6);
   db1_copy_col_text(it->created_at, sizeof(it->created_at), stmt, 7);
}

/* --- Public API --- */

int db1_diagnose_start(const char *symptom)
{
   if (!symptom || !symptom[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, "INSERT INTO diagnoses(symptom, status) VALUES(?, 'active')", -1,
                          &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, symptom, -1, SQLITE_STATIC);
   sqlite3_step(stmt);
   int changes = sqlite3_changes(db);
   int id = (int)sqlite3_last_insert_rowid(db);
   sqlite3_finalize(stmt);
   return changes > 0 ? id : -1;
}

int db1_diagnose_add_observation(int diag_id, const char *content, const char *source)
{
   return insert_item(diag_id, "observation", 0, content, source, DIAG_RANK_LOG);
}

int db1_diagnose_add_hypothesis(int diag_id, const char *content)
{
   return insert_item(diag_id, "hypothesis", 0, content, "", DIAG_RANK_SPECULATION);
}

int db1_diagnose_add_evidence(int diag_id, int hypothesis_id, const char *kind, const char *content,
                              const char *source, int rank)
{
   if (!kind)
      return -1;
   if (strcmp(kind, "evidence_for") != 0 && strcmp(kind, "evidence_against") != 0)
      return -1;
   if (hypothesis_id <= 0)
      return -1;
   return insert_item(diag_id, kind, hypothesis_id, content, source, rank);
}

int db1_diagnose_add_probe(int diag_id, int hypothesis_id, const char *content)
{
   return insert_item(diag_id, "probe", hypothesis_id, content, "", DIAG_RANK_DIRECT);
}

int db1_diagnose_get(int diag_id, diagnosis_t *out)
{
   if (!out)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(
           db,
           "SELECT id, symptom, status, conclusion, confidence, created_at, updated_at"
           " FROM diagnoses WHERE id = ?",
           -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, diag_id);
   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      row_to_diagnosis(stmt, out);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}

int db1_diagnose_list(diagnosis_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(
           db,
           "SELECT id, symptom, status, conclusion, confidence, created_at, updated_at"
           " FROM diagnoses ORDER BY updated_at DESC, id DESC",
           -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   int n = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && n < max)
   {
      row_to_diagnosis(stmt, &out[n]);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_diagnose_list_items(int diag_id, diagnosis_item_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(
           db,
           "SELECT id, diagnosis_id, kind, parent_id, content, source, evidence_rank, created_at"
           " FROM diagnosis_items WHERE diagnosis_id = ? ORDER BY id ASC",
           -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_int(stmt, 1, diag_id);
   int n = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && n < max)
   {
      row_to_item(stmt, &out[n]);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_diagnose_list_hypotheses(int diag_id, diagnosis_item_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(
           db,
           "SELECT id, diagnosis_id, kind, parent_id, content, source, evidence_rank, created_at"
           " FROM diagnosis_items WHERE diagnosis_id = ? AND kind = 'hypothesis' ORDER BY id ASC",
           -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_int(stmt, 1, diag_id);
   int n = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && n < max)
   {
      row_to_item(stmt, &out[n]);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

/* --- Ranking / confidence --- */

static double rank_weight(int rank)
{
   switch (clamp_rank(rank))
   {
   case DIAG_RANK_DIRECT:
      return 1.0;
   case DIAG_RANK_LOG:
      return 0.6;
   case DIAG_RANK_CODE:
      return 0.3;
   default:
      return 0.1;
   }
}

static int cmp_ranking_desc(const void *a, const void *b)
{
   const diagnosis_ranking_t *ra = (const diagnosis_ranking_t *)a;
   const diagnosis_ranking_t *rb = (const diagnosis_ranking_t *)b;
   if (rb->confidence > ra->confidence)
      return 1;
   if (rb->confidence < ra->confidence)
      return -1;
   return ra->hypothesis.id - rb->hypothesis.id;
}

int db1_diagnose_rank_hypotheses(int diag_id, diagnosis_ranking_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   diagnosis_item_t hyps[64];
   int hyp_count = db1_diagnose_list_hypotheses(diag_id, hyps, 64);
   if (hyp_count == 0)
      return 0;
   if (hyp_count > max)
      hyp_count = max;

   for (int i = 0; i < hyp_count; i++)
   {
      out[i].hypothesis = hyps[i];
      out[i].evidence_for_count = 0;
      out[i].evidence_against_count = 0;
      out[i].strongest_for_rank = 0;
      out[i].strongest_against_rank = 0;
      out[i].confidence = 0.0;
   }

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(
           db,
           "SELECT parent_id, kind, evidence_rank FROM diagnosis_items"
           " WHERE diagnosis_id = ? AND (kind = 'evidence_for' OR kind = 'evidence_against')",
           -1, &stmt, NULL) != SQLITE_OK)
      return hyp_count;
   sqlite3_bind_int(stmt, 1, diag_id);

   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      int parent = sqlite3_column_int(stmt, 0);
      const unsigned char *kind = sqlite3_column_text(stmt, 1);
      int rank = clamp_rank(sqlite3_column_int(stmt, 2));

      for (int i = 0; i < hyp_count; i++)
      {
         if (out[i].hypothesis.id != parent)
            continue;
         if (kind && strcmp((const char *)kind, "evidence_for") == 0)
         {
            out[i].evidence_for_count++;
            if (out[i].strongest_for_rank == 0 || rank < out[i].strongest_for_rank)
               out[i].strongest_for_rank = rank;
            out[i].confidence += rank_weight(rank);
         }
         else if (kind && strcmp((const char *)kind, "evidence_against") == 0)
         {
            out[i].evidence_against_count++;
            if (out[i].strongest_against_rank == 0 || rank < out[i].strongest_against_rank)
               out[i].strongest_against_rank = rank;
            out[i].confidence -= rank_weight(rank);
         }
         break;
      }
   }
   sqlite3_finalize(stmt);

   for (int i = 0; i < hyp_count; i++)
   {
      double x = out[i].confidence;
      if (x > 20.0)
         x = 20.0;
      if (x < -20.0)
         x = -20.0;
      out[i].confidence = 1.0 / (1.0 + exp(-x));
   }

   qsort(out, hyp_count, sizeof(diagnosis_ranking_t), cmp_ranking_desc);
   return hyp_count;
}

int db1_diagnose_conclude(int diag_id, const char *conclusion, double confidence)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(
           db,
           "UPDATE diagnoses SET status = 'concluded', conclusion = ?, confidence = ?,"
           " updated_at = datetime('now') WHERE id = ? AND status = 'active'",
           -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, conclusion ? conclusion : "", -1, SQLITE_STATIC);
   sqlite3_bind_double(stmt, 2, confidence);
   sqlite3_bind_int(stmt, 3, diag_id);
   sqlite3_step(stmt);
   int changes = sqlite3_changes(db);
   sqlite3_finalize(stmt);
   return changes > 0 ? 0 : -1;
}

int db1_diagnose_abandon(int diag_id)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db,
                          "UPDATE diagnoses SET status = 'abandoned', updated_at = datetime('now')"
                          " WHERE id = ? AND status = 'active'",
                          -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, diag_id);
   sqlite3_step(stmt);
   int changes = sqlite3_changes(db);
   sqlite3_finalize(stmt);
   return changes > 0 ? 0 : -1;
}

/* --- JSON / rendering --- */

static cJSON *item_to_json(const diagnosis_item_t *it)
{
   cJSON *obj = cJSON_CreateObject();
   cJSON_AddNumberToObject(obj, "id", it->id);
   cJSON_AddStringToObject(obj, "kind", it->kind);
   cJSON_AddNumberToObject(obj, "parent_id", it->parent_id);
   cJSON_AddStringToObject(obj, "content", it->content);
   cJSON_AddStringToObject(obj, "source", it->source);
   cJSON_AddNumberToObject(obj, "evidence_rank", it->evidence_rank);
   cJSON_AddStringToObject(obj, "created_at", it->created_at);
   return obj;
}

static cJSON *diagnosis_to_json_summary(const diagnosis_t *d)
{
   cJSON *obj = cJSON_CreateObject();
   cJSON_AddNumberToObject(obj, "id", d->id);
   cJSON_AddStringToObject(obj, "symptom", d->symptom);
   cJSON_AddStringToObject(obj, "status", d->status);
   cJSON_AddStringToObject(obj, "conclusion", d->conclusion);
   cJSON_AddNumberToObject(obj, "confidence", d->confidence);
   cJSON_AddStringToObject(obj, "created_at", d->created_at);
   cJSON_AddStringToObject(obj, "updated_at", d->updated_at);
   return obj;
}

char *db1_diagnose_json_full(int diag_id)
{
   diagnosis_t d;
   if (db1_diagnose_get(diag_id, &d) != 0)
      return NULL;

   cJSON *obj = diagnosis_to_json_summary(&d);

   diagnosis_item_t items[512];
   int n = db1_diagnose_list_items(diag_id, items, 512);
   cJSON *arr = cJSON_AddArrayToObject(obj, "items");
   for (int i = 0; i < n; i++)
      cJSON_AddItemToArray(arr, item_to_json(&items[i]));

   diagnosis_ranking_t rankings[64];
   int r = db1_diagnose_rank_hypotheses(diag_id, rankings, 64);
   cJSON *rarr = cJSON_AddArrayToObject(obj, "hypotheses_ranked");
   for (int i = 0; i < r; i++)
   {
      cJSON *h = cJSON_CreateObject();
      cJSON_AddNumberToObject(h, "id", rankings[i].hypothesis.id);
      cJSON_AddStringToObject(h, "content", rankings[i].hypothesis.content);
      cJSON_AddNumberToObject(h, "confidence", rankings[i].confidence);
      cJSON_AddNumberToObject(h, "evidence_for", rankings[i].evidence_for_count);
      cJSON_AddNumberToObject(h, "evidence_against", rankings[i].evidence_against_count);
      cJSON_AddNumberToObject(h, "strongest_for_rank", rankings[i].strongest_for_rank);
      cJSON_AddNumberToObject(h, "strongest_against_rank", rankings[i].strongest_against_rank);
      cJSON_AddItemToArray(rarr, h);
   }

   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return json;
}

char *db1_diagnose_json_list(void)
{
   diagnosis_t rows[128];
   int n = db1_diagnose_list(rows, 128);
   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < n; i++)
      cJSON_AddItemToArray(arr, diagnosis_to_json_summary(&rows[i]));
   char *json = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return json;
}

static const char *rank_label(int rank)
{
   switch (clamp_rank(rank))
   {
   case DIAG_RANK_DIRECT:
      return "direct";
   case DIAG_RANK_LOG:
      return "log";
   case DIAG_RANK_CODE:
      return "code";
   default:
      return "speculation";
   }
}

char *db1_diagnose_render_status(int diag_id)
{
   diagnosis_t d;
   if (db1_diagnose_get(diag_id, &d) != 0)
      return NULL;

   dstr_t out;
   dstr_init(&out);
   dstr_appendf(&out, "# Diagnosis #%d: %s\n", d.id, d.symptom);
   dstr_appendf(&out, "Status: %s", d.status);
   if (d.confidence > 0.0 && strcmp(d.status, "concluded") == 0)
      dstr_appendf(&out, " (confidence %.2f)", d.confidence);
   dstr_append_char(&out, '\n');
   if (d.conclusion[0])
      dstr_appendf(&out, "Conclusion: %s\n", d.conclusion);

   diagnosis_item_t items[512];
   int total = db1_diagnose_list_items(diag_id, items, 512);

   dstr_append_str(&out, "\n## Observations\n");
   int obs = 0;
   for (int i = 0; i < total; i++)
      if (strcmp(items[i].kind, "observation") == 0)
      {
         obs++;
         dstr_appendf(&out, "%d. [%s] %s", obs, rank_label(items[i].evidence_rank),
                      items[i].content);
         if (items[i].source[0])
            dstr_appendf(&out, "  (%s)", items[i].source);
         dstr_append_char(&out, '\n');
      }
   if (obs == 0)
      dstr_append_str(&out, "(none)\n");

   diagnosis_ranking_t rankings[64];
   int r = db1_diagnose_rank_hypotheses(diag_id, rankings, 64);
   dstr_append_str(&out, "\n## Hypotheses (ranked by evidence)\n");
   if (r == 0)
      dstr_append_str(&out, "(none)\n");
   for (int i = 0; i < r; i++)
   {
      dstr_appendf(&out, "### H%d (id=%d, confidence %.2f): %s\n", i + 1, rankings[i].hypothesis.id,
                   rankings[i].confidence, rankings[i].hypothesis.content);
      for (int j = 0; j < total; j++)
      {
         if (items[j].parent_id != rankings[i].hypothesis.id)
            continue;
         if (strcmp(items[j].kind, "evidence_for") == 0)
            dstr_appendf(&out, "  + [%s] %s\n", rank_label(items[j].evidence_rank),
                         items[j].content);
         else if (strcmp(items[j].kind, "evidence_against") == 0)
            dstr_appendf(&out, "  - [%s] %s\n", rank_label(items[j].evidence_rank),
                         items[j].content);
         else if (strcmp(items[j].kind, "probe") == 0)
            dstr_appendf(&out, "  -> probe: %s\n", items[j].content);
      }
   }

   diagnosis_probe_suggestion_t probes[DIAG_MAX_SUGGEST];
   int probe_count = db1_diagnose_suggest_probes(diag_id, probes, DIAG_MAX_SUGGEST);
   if (probe_count > 0)
   {
      dstr_append_str(&out, "\n## Suggested Probes\n");
      for (int i = 0; i < probe_count; i++)
         dstr_appendf(&out, "%d. %s\n", i + 1, probes[i].suggestion);
   }

   return dstr_steal(&out);
}

static const char *weakest_evidence_label(const diagnosis_ranking_t *r)
{
   int weakest_for = (r->strongest_for_rank > 0) ? r->strongest_for_rank : DIAG_RANK_SPECULATION;
   int weakest_against =
       (r->strongest_against_rank > 0) ? r->strongest_against_rank : DIAG_RANK_SPECULATION;
   int weakest = weakest_for > weakest_against ? weakest_for : weakest_against;
   if (weakest <= DIAG_RANK_LOG)
      return "log/metric";
   if (weakest <= DIAG_RANK_CODE)
      return "code";
   return "speculation";
}

int db1_diagnose_suggest_probes(int diag_id, diagnosis_probe_suggestion_t *out, int max)
{
   if (!out || max <= 0)
      return 0;

   diagnosis_ranking_t rankings[64];
   int r = db1_diagnose_rank_hypotheses(diag_id, rankings, 64);
   if (r == 0)
      return 0;

   int count = 0;

   for (int i = 0; i < r && count < max; i++)
   {
      double c = rankings[i].confidence;
      if (c >= 0.35 && c <= 0.65 && rankings[i].evidence_against_count == 0)
      {
         out[count].hypothesis_a_id = rankings[i].hypothesis.id;
         out[count].hypothesis_b_id = 0;
         snprintf(out[count].suggestion, DIAG_PROBE_SUGGESTION_LEN,
                  "H%d (\"%.*s\") is weakly evidenced (confidence %.2f, %s-level). "
                  "Collect a direct experiment or log correlation to strengthen or refute it.",
                  i + 1, 60, rankings[i].hypothesis.content, c,
                  weakest_evidence_label(&rankings[i]));
         count++;
      }
   }

   for (int i = 0; i < r && count < max; i++)
   {
      for (int j = i + 1; j < r && count < max; j++)
      {
         double diff = rankings[i].confidence - rankings[j].confidence;
         if (diff < 0.0)
            diff = -diff;
         if (diff > DIAG_SUGGEST_BALANCE_THRESHOLD)
            continue;
         if (rankings[i].confidence > 0.80 || rankings[j].confidence > 0.80)
            continue;

         out[count].hypothesis_a_id = rankings[i].hypothesis.id;
         out[count].hypothesis_b_id = rankings[j].hypothesis.id;

         const char *ev_label = weakest_evidence_label(&rankings[i]);
         if (strcmp(ev_label, "speculation") == 0)
         {
            snprintf(out[count].suggestion, DIAG_PROBE_SUGGESTION_LEN,
                     "H%d and H%d are tied at %.2f vs %.2f (speculation only). "
                     "Run a direct experiment — a targeted test or controlled observation — "
                     "to discriminate \"%.*s\" from \"%.*s\".",
                     i + 1, j + 1, rankings[i].confidence, rankings[j].confidence, 50,
                     rankings[i].hypothesis.content, 50, rankings[j].hypothesis.content);
         }
         else if (strcmp(ev_label, "log/metric") == 0)
         {
            snprintf(out[count].suggestion, DIAG_PROBE_SUGGESTION_LEN,
                     "H%d and H%d are tied at %.2f vs %.2f (log-level evidence). "
                     "Collect time-correlated metrics or traces at the event boundary "
                     "to distinguish \"%.*s\" from \"%.*s\".",
                     i + 1, j + 1, rankings[i].confidence, rankings[j].confidence, 50,
                     rankings[i].hypothesis.content, 50, rankings[j].hypothesis.content);
         }
         else
         {
            snprintf(out[count].suggestion, DIAG_PROBE_SUGGESTION_LEN,
                     "H%d and H%d are tied at %.2f vs %.2f (code-level evidence). "
                     "Trace the code path at the decision point to confirm "
                     "\"%.*s\" over \"%.*s\".",
                     i + 1, j + 1, rankings[i].confidence, rankings[j].confidence, 50,
                     rankings[i].hypothesis.content, 50, rankings[j].hypothesis.content);
         }
         count++;
      }
   }

   return count;
}
