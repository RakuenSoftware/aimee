/* db1/db1_cron_jobs.c: cron job mirror and run history. */

#include "db1_cron_jobs.h"
#include "db1_internal.h"

#include "cJSON.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void bind_text(sqlite3_stmt *stmt, int idx, const char *value)
{
   sqlite3_bind_text(stmt, idx, value ? value : "", -1, SQLITE_TRANSIENT);
}

static void add_col_string(cJSON *obj, const char *key, sqlite3_stmt *stmt, int col)
{
   const unsigned char *v = sqlite3_column_text(stmt, col);
   cJSON_AddStringToObject(obj, key, v ? (const char *)v : "");
}

static void cron_job_parse_skills_csv(cron_job_t *job, const char *csv)
{
   if (!job || !csv || !csv[0])
      return;

   char buf[CRON_JOB_MAX_SKILLS * (CRON_JOB_MAX_SKILL_NAME + 1)];
   snprintf(buf, sizeof(buf), "%s", csv);
   char *save = NULL;
   for (char *tok = strtok_r(buf, ",", &save); tok && job->skill_count < CRON_JOB_MAX_SKILLS;
        tok = strtok_r(NULL, ",", &save))
   {
      while (*tok == ' ' || *tok == '\t')
         tok++;
      if (!tok[0])
         continue;
      snprintf(job->skills[job->skill_count], sizeof(job->skills[job->skill_count]), "%s", tok);
      job->skill_count++;
   }
}

static void cron_job_from_stmt(sqlite3_stmt *stmt, cron_job_t *out)
{
   memset(out, 0, sizeof(*out));
   const unsigned char *v;

   v = sqlite3_column_text(stmt, 0);
   snprintf(out->id, sizeof(out->id), "%s", v ? (const char *)v : "");
   v = sqlite3_column_text(stmt, 1);
   snprintf(out->schedule, sizeof(out->schedule), "%s", v ? (const char *)v : "");
   v = sqlite3_column_text(stmt, 2);
   snprintf(out->mode, sizeof(out->mode), "%s",
            v && ((const char *)v)[0] ? (const char *)v : "llm");
   v = sqlite3_column_text(stmt, 3);
   snprintf(out->script, sizeof(out->script), "%s", v ? (const char *)v : "");
   v = sqlite3_column_text(stmt, 4);
   snprintf(out->prompt, sizeof(out->prompt), "%s", v ? (const char *)v : "");
   v = sqlite3_column_text(stmt, 5);
   cron_job_parse_skills_csv(out, v ? (const char *)v : "");
   v = sqlite3_column_text(stmt, 6);
   snprintf(out->workdir, sizeof(out->workdir), "%s", v ? (const char *)v : "");
   v = sqlite3_column_text(stmt, 7);
   snprintf(out->deliver_target, sizeof(out->deliver_target), "%s", v ? (const char *)v : "");
   out->deliver_only_if_changed = sqlite3_column_int(stmt, 8);
   out->deliver_first_run_silent = sqlite3_column_int(stmt, 9);
   v = sqlite3_column_text(stmt, 10);
   snprintf(out->context_from, sizeof(out->context_from), "%s", v ? (const char *)v : "");
   v = sqlite3_column_text(stmt, 11);
   snprintf(out->when_context_contains, sizeof(out->when_context_contains), "%s",
            v ? (const char *)v : "");
   out->pre_wake_gate = sqlite3_column_int(stmt, 12);
   out->enabled = sqlite3_column_int(stmt, 13);
}

static const char *cron_job_select_cols(void)
{
   return "id, schedule, mode, script, prompt, skills_csv, workdir, deliver_target,"
          " deliver_only_if_changed, deliver_first_run_silent, context_from,"
          " when_context_contains, pre_wake_gate, enabled";
}

int db1_cron_job_upsert(const cron_job_t *job)
{
   if (!job || !job->id[0] || !job->schedule[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT INTO cron_jobs"
       " (id, schedule, mode, script, prompt, skills_csv, workdir, deliver_target,"
       "  deliver_only_if_changed, deliver_first_run_silent, context_from,"
       "  when_context_contains, pre_wake_gate, enabled, created_at)"
       " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, strftime('%s','now'))"
       " ON CONFLICT(id) DO UPDATE SET"
       "  schedule=excluded.schedule,"
       "  mode=excluded.mode,"
       "  script=excluded.script,"
       "  prompt=excluded.prompt,"
       "  skills_csv=excluded.skills_csv,"
       "  workdir=excluded.workdir,"
       "  deliver_target=excluded.deliver_target,"
       "  deliver_only_if_changed=excluded.deliver_only_if_changed,"
       "  deliver_first_run_silent=excluded.deliver_first_run_silent,"
       "  context_from=excluded.context_from,"
       "  when_context_contains=excluded.when_context_contains,"
       "  pre_wake_gate=excluded.pre_wake_gate,"
       "  enabled=excluded.enabled";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   char skills_csv[CRON_JOB_MAX_SKILLS * (CRON_JOB_MAX_SKILL_NAME + 1)];
   skills_csv[0] = '\0';
   for (int i = 0; i < job->skill_count && i < CRON_JOB_MAX_SKILLS; i++)
   {
      if (!job->skills[i][0])
         continue;
      if (skills_csv[0])
         strncat(skills_csv, ",", sizeof(skills_csv) - strlen(skills_csv) - 1);
      strncat(skills_csv, job->skills[i], sizeof(skills_csv) - strlen(skills_csv) - 1);
   }

   bind_text(stmt, 1, job->id);
   bind_text(stmt, 2, job->schedule);
   bind_text(stmt, 3, job->mode[0] ? job->mode : "llm");
   bind_text(stmt, 4, job->script);
   bind_text(stmt, 5, job->prompt);
   bind_text(stmt, 6, skills_csv);
   bind_text(stmt, 7, job->workdir);
   bind_text(stmt, 8, job->deliver_target);
   sqlite3_bind_int(stmt, 9, job->deliver_only_if_changed);
   sqlite3_bind_int(stmt, 10, job->deliver_first_run_silent);
   bind_text(stmt, 11, job->context_from);
   bind_text(stmt, 12, job->when_context_contains);
   sqlite3_bind_int(stmt, 13, job->pre_wake_gate);
   sqlite3_bind_int(stmt, 14, job->enabled);

   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_cron_job_get(const char *job_id, cron_job_t *out)
{
   if (!job_id || !job_id[0] || !out)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   char sql[512];
   snprintf(sql, sizeof(sql), "SELECT %s FROM cron_jobs WHERE id = ?", cron_job_select_cols());
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   bind_text(stmt, 1, job_id);
   int rc = sqlite3_step(stmt);
   if (rc == SQLITE_ROW)
      cron_job_from_stmt(stmt, out);
   sqlite3_finalize(stmt);
   return rc == SQLITE_ROW ? 0 : -1;
}

int db1_cron_jobs_load(cron_job_t *out, int max, int enabled_only)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   char sql[512];
   snprintf(sql, sizeof(sql), "SELECT %s FROM cron_jobs%s ORDER BY id", cron_job_select_cols(),
            enabled_only ? " WHERE enabled != 0" : "");
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      cron_job_from_stmt(stmt, &out[n]);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_cron_job_set_enabled(const char *job_id, int enabled)
{
   if (!job_id || !job_id[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *sql = "UPDATE cron_jobs SET enabled = ? WHERE id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, enabled ? 1 : 0);
   bind_text(stmt, 2, job_id);
   int rc = sqlite3_step(stmt);
   int changed = sqlite3_changes(db);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE && changed > 0) ? 0 : -1;
}

int db1_cron_jobs_set_enabled_all(int enabled)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *sql = "UPDATE cron_jobs SET enabled = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, enabled ? 1 : 0);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_cron_job_delete(const char *job_id)
{
   if (!job_id || !job_id[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *runs = "DELETE FROM cron_job_runs WHERE job_id = ?";
   if (sqlite3_prepare_v2(db, runs, -1, &stmt, NULL) == SQLITE_OK)
   {
      bind_text(stmt, 1, job_id);
      (void)sqlite3_step(stmt);
      sqlite3_finalize(stmt);
   }

   static const char *jobs = "DELETE FROM cron_jobs WHERE id = ?";
   if (sqlite3_prepare_v2(db, jobs, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   bind_text(stmt, 1, job_id);
   int rc = sqlite3_step(stmt);
   int changed = sqlite3_changes(db);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE && changed > 0) ? 0 : -1;
}

int db1_cron_job_record_run(const char *job_id, const char *status, int silent, int delivered,
                            const char *output, const char *error, const char *output_hash)
{
   if (!job_id || !job_id[0] || !status || !status[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT INTO cron_job_runs"
       " (job_id, started_at, completed_at, status, silent, delivered, output, error, output_hash)"
       " VALUES (?, strftime('%s','now'), strftime('%s','now'), ?, ?, ?, ?, ?, ?)";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   bind_text(stmt, 1, job_id);
   bind_text(stmt, 2, status);
   sqlite3_bind_int(stmt, 3, silent ? 1 : 0);
   sqlite3_bind_int(stmt, 4, delivered ? 1 : 0);
   bind_text(stmt, 5, output);
   bind_text(stmt, 6, error);
   bind_text(stmt, 7, output_hash);
   int rc = sqlite3_step(stmt);
   int run_id = (rc == SQLITE_DONE) ? (int)sqlite3_last_insert_rowid(db) : -1;
   sqlite3_finalize(stmt);
   if (run_id < 0)
      return -1;

   static const char *upd =
       "UPDATE cron_jobs SET last_run_at = strftime('%s','now'), last_run_status = ?,"
       " last_run_output_hash = ? WHERE id = ?";
   if (sqlite3_prepare_v2(db, upd, -1, &stmt, NULL) == SQLITE_OK)
   {
      bind_text(stmt, 1, status);
      bind_text(stmt, 2, output_hash);
      bind_text(stmt, 3, job_id);
      (void)sqlite3_step(stmt);
      sqlite3_finalize(stmt);
   }
   return run_id;
}

char *db1_cron_jobs_list_json(void)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return NULL;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT id, schedule, mode, workdir, deliver_target, enabled,"
                            " COALESCE(last_run_at,0), COALESCE(last_run_status,'')"
                            " FROM cron_jobs ORDER BY id";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return NULL;

   cJSON *arr = cJSON_CreateArray();
   if (!arr)
   {
      sqlite3_finalize(stmt);
      return NULL;
   }
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      cJSON *obj = cJSON_CreateObject();
      if (!obj)
         break;
      add_col_string(obj, "id", stmt, 0);
      add_col_string(obj, "schedule", stmt, 1);
      add_col_string(obj, "mode", stmt, 2);
      add_col_string(obj, "workdir", stmt, 3);
      add_col_string(obj, "deliver_target", stmt, 4);
      cJSON_AddBoolToObject(obj, "enabled", sqlite3_column_int(stmt, 5) != 0);
      cJSON_AddNumberToObject(obj, "last_run_at", sqlite3_column_int64(stmt, 6));
      add_col_string(obj, "last_run_status", stmt, 7);
      cJSON_AddItemToArray(arr, obj);
   }
   sqlite3_finalize(stmt);
   char *out = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return out;
}

char *db1_cron_job_history_json(const char *job_id, int limit)
{
   if (!job_id || !job_id[0])
      return NULL;
   if (limit <= 0)
      limit = 20;
   if (limit > 200)
      limit = 200;
   sqlite3 *db = db1_conn();
   if (!db)
      return NULL;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT id, job_id, started_at, COALESCE(completed_at,0), status, silent, delivered,"
       " COALESCE(output_hash,''), COALESCE(output,''), COALESCE(error,'')"
       " FROM cron_job_runs WHERE job_id = ? ORDER BY id DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return NULL;
   bind_text(stmt, 1, job_id);
   sqlite3_bind_int(stmt, 2, limit);

   cJSON *arr = cJSON_CreateArray();
   if (!arr)
   {
      sqlite3_finalize(stmt);
      return NULL;
   }
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      cJSON *obj = cJSON_CreateObject();
      if (!obj)
         break;
      cJSON_AddNumberToObject(obj, "id", sqlite3_column_int(stmt, 0));
      add_col_string(obj, "job_id", stmt, 1);
      cJSON_AddNumberToObject(obj, "started_at", sqlite3_column_int64(stmt, 2));
      cJSON_AddNumberToObject(obj, "completed_at", sqlite3_column_int64(stmt, 3));
      add_col_string(obj, "status", stmt, 4);
      cJSON_AddBoolToObject(obj, "silent", sqlite3_column_int(stmt, 5) != 0);
      cJSON_AddBoolToObject(obj, "delivered", sqlite3_column_int(stmt, 6) != 0);
      add_col_string(obj, "output_hash", stmt, 7);
      add_col_string(obj, "output", stmt, 8);
      add_col_string(obj, "error", stmt, 9);
      cJSON_AddItemToArray(arr, obj);
   }
   sqlite3_finalize(stmt);
   char *out = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return out;
}

static char *single_text_query(const char *sql, const char *job_id)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return NULL;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return NULL;
   bind_text(stmt, 1, job_id);
   char *out = NULL;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *v = sqlite3_column_text(stmt, 0);
      if (v)
         out = strdup((const char *)v);
   }
   sqlite3_finalize(stmt);
   return out;
}

char *db1_cron_job_latest_output(const char *job_id)
{
   if (!job_id || !job_id[0])
      return NULL;
   return single_text_query("SELECT COALESCE(output,'') FROM cron_job_runs"
                            " WHERE job_id = ? ORDER BY id DESC LIMIT 1",
                            job_id);
}

char *db1_cron_job_last_output_hash(const char *job_id)
{
   if (!job_id || !job_id[0])
      return NULL;
   return single_text_query("SELECT COALESCE(last_run_output_hash,'') FROM cron_jobs WHERE id = ?",
                            job_id);
}
