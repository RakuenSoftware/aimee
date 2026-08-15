/* db1/clarify.c: planning-preparation clarification subsystem.
 *
 * SQL uses the module-private sqlite3 connection via db1_conn(). Pure
 * helpers (scoring, weakest-dimension selection, question generation,
 * spec crystallization, JSON serialisation) live here alongside the DB
 * operations because they operate on clarify_session_t in-memory. */

#include "clarify.h"
#include "db1_internal.h"
#include "dstr.h"
#include "cJSON.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Dimension metadata --- */

static const struct
{
   const char *name;
   const char *question_template;
} dims[CLARIFY_NUM_DIMS] = {
    {"scope", "What is in scope for this task, and what should explicitly be left out?"},
    {"success_criteria",
     "How will you know this task is complete? What does a successful outcome look like?"},
    {"constraints",
     "Are there any hard constraints, requirements, or things that must not change?"},
    {"approach", "Do you have a preferred approach, technology, or pattern for this task?"},
    {"context", "What relevant context about the existing system should I know before starting?"},
};

static const char *dim_name(int d)
{
   if (d < 0 || d >= CLARIFY_NUM_DIMS)
      return "unknown";
   return dims[d].name;
}

static int dim_index(const char *name)
{
   if (!name)
      return -1;
   for (int i = 0; i < CLARIFY_NUM_DIMS; i++)
      if (strcmp(dims[i].name, name) == 0)
         return i;
   return -1;
}

/* --- Scoring --- */

static float dim_score(const clarify_session_t *s, int dim)
{
   if (!s)
      return 0.0f;
   int answered = 0;
   for (int i = 0; i < s->qa_count; i++)
   {
      if (s->qa[i].answered && dim_index(s->qa[i].dimension) == dim)
         answered++;
   }
   if (answered == 0)
      return 0.0f;
   if (answered == 1)
      return 0.7f;
   return 1.0f;
}

float db1_clarify_score(const clarify_session_t *s)
{
   if (!s)
      return 0.0f;

   size_t len = strlen(s->description);
   float base;
   if (len >= 200)
      base = 0.30f;
   else if (len >= 80)
      base = 0.15f;
   else
      base = 0.05f;

   float dim_total = 0.0f;
   for (int i = 0; i < CLARIFY_NUM_DIMS; i++)
      dim_total += dim_score(s, i);

   float qa_contribution = (dim_total / CLARIFY_NUM_DIMS) * 0.70f;
   float score = base + qa_contribution;
   if (score > 1.0f)
      score = 1.0f;
   return score;
}

void db1_clarify_weakest_dim(const clarify_session_t *s, char *dim_out, size_t len)
{
   if (!s || !dim_out || len == 0)
      return;

   int weakest = 0;
   float weakest_score = dim_score(s, 0);
   for (int i = 1; i < CLARIFY_NUM_DIMS; i++)
   {
      int pending = 0;
      for (int j = 0; j < s->qa_count; j++)
      {
         if (!s->qa[j].answered && dim_index(s->qa[j].dimension) == i)
         {
            pending = 1;
            break;
         }
      }
      if (pending)
         continue;

      float sc = dim_score(s, i);
      if (sc < weakest_score)
      {
         weakest_score = sc;
         weakest = i;
      }
   }
   snprintf(dim_out, len, "%s", dim_name(weakest));
}

int db1_clarify_next_question(const clarify_session_t *s, char *q_out, size_t q_len, char *dim_out,
                              size_t dim_len)
{
   if (!s || !q_out || !dim_out)
      return -1;

   if (s->status == CLARIFY_READY || db1_clarify_score(s) >= CLARIFY_READY_SCORE)
      return 1;

   if (s->qa_count >= CLARIFY_MAX_QA)
      return -1;

   char weakest[CLARIFY_DIM_NAME_LEN];
   db1_clarify_weakest_dim(s, weakest, sizeof(weakest));

   int d = dim_index(weakest);
   if (d < 0)
      return -1;

   snprintf(q_out, q_len, "%s", dims[d].question_template);
   snprintf(dim_out, dim_len, "%s", dims[d].name);
   return 0;
}

char *db1_clarify_crystallize(const clarify_session_t *s)
{
   if (!s)
      return NULL;

   dstr_t out;
   dstr_init(&out);
   dstr_appendf(&out, "# Task Specification\n\n## Task\n%s\n", s->description);

   if (s->qa_count > 0)
   {
      dstr_appendf(&out, "\n## Clarifications\n");
      for (int i = 0; i < s->qa_count; i++)
      {
         if (!s->qa[i].answered)
            continue;
         dstr_appendf(&out, "\n**%s**: %s\n> %s\n", s->qa[i].dimension, s->qa[i].question,
                      s->qa[i].answer);
      }
   }

   return dstr_steal(&out);
}

/* --- DB helpers (private) --- */

static void row_to_session(sqlite3_stmt *stmt, clarify_session_t *s)
{
   s->id = sqlite3_column_int(stmt, 0);
   db1_copy_col_text(s->description, sizeof(s->description), stmt, 1);
   const unsigned char *st = sqlite3_column_text(stmt, 2);
   if (st && strcmp((const char *)st, "ready") == 0)
      s->status = CLARIFY_READY;
   else if (st && strcmp((const char *)st, "cancelled") == 0)
      s->status = CLARIFY_CANCELLED;
   else
      s->status = CLARIFY_OPEN;
   s->score = (float)sqlite3_column_double(stmt, 3);
   db1_copy_col_text(s->spec, sizeof(s->spec), stmt, 4);
   db1_copy_col_text(s->created_at, sizeof(s->created_at), stmt, 5);
   db1_copy_col_text(s->updated_at, sizeof(s->updated_at), stmt, 6);
}

static int load_qa(sqlite3 *db, clarify_session_t *s)
{
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db,
                          "SELECT dimension, question, answer, answered, seq"
                          " FROM clarify_qa WHERE session_id = ? ORDER BY seq ASC",
                          -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, s->id);
   s->qa_count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && s->qa_count < CLARIFY_MAX_QA)
   {
      clarify_qa_t *qa = &s->qa[s->qa_count];
      db1_copy_col_text(qa->dimension, sizeof(qa->dimension), stmt, 0);
      db1_copy_col_text(qa->question, sizeof(qa->question), stmt, 1);
      db1_copy_col_text(qa->answer, sizeof(qa->answer), stmt, 2);
      qa->answered = sqlite3_column_int(stmt, 3);
      qa->seq = sqlite3_column_int(stmt, 4);
      s->qa_count++;
   }
   sqlite3_finalize(stmt);
   return 0;
}

static int save_session_status(sqlite3 *db, int id, clarify_status_t status, float score,
                               const char *spec)
{
   const char *status_str = (status == CLARIFY_READY)       ? "ready"
                            : (status == CLARIFY_CANCELLED) ? "cancelled"
                                                            : "open";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db,
                          "UPDATE clarify_sessions SET status=?, score=?, spec=?,"
                          " updated_at=datetime('now') WHERE id=?",
                          -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, status_str, -1, SQLITE_STATIC);
   sqlite3_bind_double(stmt, 2, (double)score);
   sqlite3_bind_text(stmt, 3, spec ? spec : "", -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 4, id);
   sqlite3_step(stmt);
   int changes = sqlite3_changes(db);
   sqlite3_finalize(stmt);
   return changes > 0 ? 0 : -1;
}

/* --- Public API --- */

int db1_clarify_start(const char *description, clarify_session_t *out)
{
   if (!description || !description[0])
      return -1;
   if (strlen(description) >= CLARIFY_DESC_LEN)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db,
                          "INSERT INTO clarify_sessions(description, status, score, spec)"
                          " VALUES(?, 'open', 0, '')",
                          -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, description, -1, SQLITE_STATIC);
   sqlite3_step(stmt);
   int changes = sqlite3_changes(db);
   int id = (int)sqlite3_last_insert_rowid(db);
   sqlite3_finalize(stmt);

   if (changes <= 0)
      return -1;

   if (out)
   {
      memset(out, 0, sizeof(*out));
      if (db1_clarify_get(id, out) != 0)
         return -1;
      char q[CLARIFY_TEXT_LEN], dim[CLARIFY_DIM_NAME_LEN];
      if (db1_clarify_next_question(out, q, sizeof(q), dim, sizeof(dim)) == 0)
      {
         sqlite3_stmt *qs = NULL;
         if (sqlite3_prepare_v2(db,
                                "INSERT INTO clarify_qa(session_id, dimension, question,"
                                " answer, answered, seq) VALUES(?,?,?,'',0,?)",
                                -1, &qs, NULL) == SQLITE_OK)
         {
            sqlite3_bind_int(qs, 1, id);
            sqlite3_bind_text(qs, 2, dim, -1, SQLITE_STATIC);
            sqlite3_bind_text(qs, 3, q, -1, SQLITE_STATIC);
            sqlite3_bind_int(qs, 4, 0);
            sqlite3_step(qs);
            sqlite3_finalize(qs);
         }
         db1_clarify_get(id, out);
      }
   }

   return id;
}

int db1_clarify_get(int id, clarify_session_t *out)
{
   if (!out)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db,
                          "SELECT id, description, status, score, spec, created_at, updated_at"
                          " FROM clarify_sessions WHERE id=?",
                          -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, id);
   int rc = sqlite3_step(stmt);
   if (rc != SQLITE_ROW)
   {
      sqlite3_finalize(stmt);
      return -1;
   }
   memset(out, 0, sizeof(*out));
   row_to_session(stmt, out);
   sqlite3_finalize(stmt);
   return load_qa(db, out);
}

int db1_clarify_answer(int id, const char *answer, clarify_session_t *out)
{
   if (!answer || !answer[0])
      return -1;
   if (strlen(answer) >= CLARIFY_TEXT_LEN)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   clarify_session_t s;
   if (db1_clarify_get(id, &s) != 0)
      return -1;

   if (s.status != CLARIFY_OPEN)
      return -1;

   int found = -1;
   for (int i = s.qa_count - 1; i >= 0; i--)
   {
      if (!s.qa[i].answered)
      {
         found = i;
         break;
      }
   }
   if (found < 0)
      return -1;

   sqlite3_stmt *astmt = NULL;
   if (sqlite3_prepare_v2(db,
                          "UPDATE clarify_qa SET answer=?, answered=1"
                          " WHERE session_id=? AND seq=?",
                          -1, &astmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(astmt, 1, answer, -1, SQLITE_STATIC);
   sqlite3_bind_int(astmt, 2, id);
   sqlite3_bind_int(astmt, 3, s.qa[found].seq);
   sqlite3_step(astmt);
   int changes = sqlite3_changes(db);
   sqlite3_finalize(astmt);
   if (changes <= 0)
      return -1;

   if (db1_clarify_get(id, &s) != 0)
      return -1;

   float score = db1_clarify_score(&s);
   clarify_status_t new_status = s.status;
   char *spec_str = NULL;

   if (score >= CLARIFY_READY_SCORE)
   {
      new_status = CLARIFY_READY;
      spec_str = db1_clarify_crystallize(&s);
   }
   else
   {
      char q[CLARIFY_TEXT_LEN], dim[CLARIFY_DIM_NAME_LEN];
      if (db1_clarify_next_question(&s, q, sizeof(q), dim, sizeof(dim)) == 0)
      {
         int next_seq = s.qa_count;
         sqlite3_stmt *qs = NULL;
         if (sqlite3_prepare_v2(db,
                                "INSERT INTO clarify_qa(session_id, dimension, question,"
                                " answer, answered, seq) VALUES(?,?,?,'',0,?)",
                                -1, &qs, NULL) == SQLITE_OK)
         {
            sqlite3_bind_int(qs, 1, id);
            sqlite3_bind_text(qs, 2, dim, -1, SQLITE_STATIC);
            sqlite3_bind_text(qs, 3, q, -1, SQLITE_STATIC);
            sqlite3_bind_int(qs, 4, next_seq);
            sqlite3_step(qs);
            sqlite3_finalize(qs);
         }
      }
   }

   save_session_status(db, id, new_status, score, spec_str ? spec_str : "");
   free(spec_str);

   if (out)
      db1_clarify_get(id, out);

   return 0;
}

/* --- JSON serialisation --- */

char *db1_clarify_to_json(const clarify_session_t *s)
{
   if (!s)
      return NULL;

   const char *status_str = (s->status == CLARIFY_READY)       ? "ready"
                            : (s->status == CLARIFY_CANCELLED) ? "cancelled"
                                                               : "open";
   cJSON *obj = cJSON_CreateObject();
   cJSON_AddNumberToObject(obj, "id", s->id);
   cJSON_AddStringToObject(obj, "description", s->description);
   cJSON_AddStringToObject(obj, "status", status_str);
   cJSON_AddNumberToObject(obj, "score", (double)s->score);
   cJSON_AddStringToObject(obj, "spec", s->spec);
   cJSON_AddStringToObject(obj, "created_at", s->created_at);
   cJSON_AddStringToObject(obj, "updated_at", s->updated_at);

   cJSON *qa_arr = cJSON_AddArrayToObject(obj, "qa");
   for (int i = 0; i < s->qa_count; i++)
   {
      cJSON *qa = cJSON_CreateObject();
      cJSON_AddStringToObject(qa, "dimension", s->qa[i].dimension);
      cJSON_AddStringToObject(qa, "question", s->qa[i].question);
      cJSON_AddStringToObject(qa, "answer", s->qa[i].answer);
      cJSON_AddBoolToObject(qa, "answered", s->qa[i].answered);
      cJSON_AddItemToArray(qa_arr, qa);
   }

   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return json;
}
