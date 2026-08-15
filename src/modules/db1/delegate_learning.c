/* delegate_learning.c — automated delegate failure → learning feedback loop */
#include "db1_internal.h"
#include "delegate_learning.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── DB1 table: delegate_learnings ─────────────────────────────────────── */
/*
 * CREATE TABLE IF NOT EXISTS delegate_learnings (
 *   id              INTEGER PRIMARY KEY AUTOINCREMENT,
 *   created_at      TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now')),
 *   session_id      TEXT    NOT NULL DEFAULT '',
 *   role            TEXT    NOT NULL DEFAULT '',
 *   failure_mode    TEXT    NOT NULL DEFAULT 'success',
 *   lesson          TEXT    NOT NULL DEFAULT '',
 *   evidence_json   TEXT    NOT NULL DEFAULT '{}',
 *   confidence      REAL    NOT NULL DEFAULT 0.5,
 *   auto_applied    INTEGER NOT NULL DEFAULT 1,
 *   review_status   TEXT    NOT NULL DEFAULT 'pending',
 *   reviewed_at     TEXT,
 *   reviewer_notes  TEXT
 * );
 */

#define DL_TABLE_CAP 200

/* ── String conversions ────────────────────────────────────────────────── */

const char *dl_failure_mode_to_string(dl_failure_mode_t mode)
{
   switch (mode)
   {
   case DL_MODE_SUCCESS:
      return "success";
   case DL_MODE_STALL_NO_WRITES:
      return "stall/no-writes";
   case DL_MODE_STALL_SLOW_WRITES:
      return "stall/slow-writes";
   case DL_MODE_MAX_TURNS:
      return "max-turns";
   case DL_MODE_DRIFT_PREFLIGHT:
      return "drift/pre-flight";
   case DL_MODE_DRIFT_BRANCH:
      return "drift/branch";
   case DL_MODE_CANCELLED:
      return "cancelled";
   default:
      return "unknown";
   }
}

dl_failure_mode_t dl_string_to_failure_mode(const char *s)
{
   if (!s)
      return DL_MODE_SUCCESS;
   if (strcmp(s, "success") == 0)
      return DL_MODE_SUCCESS;
   if (strcmp(s, "stall/no-writes") == 0)
      return DL_MODE_STALL_NO_WRITES;
   if (strcmp(s, "stall/slow-writes") == 0)
      return DL_MODE_STALL_SLOW_WRITES;
   if (strcmp(s, "max-turns") == 0)
      return DL_MODE_MAX_TURNS;
   if (strcmp(s, "drift/pre-flight") == 0)
      return DL_MODE_DRIFT_PREFLIGHT;
   if (strcmp(s, "drift/branch") == 0)
      return DL_MODE_DRIFT_BRANCH;
   if (strcmp(s, "cancelled") == 0)
      return DL_MODE_CANCELLED;
   return DL_MODE_SUCCESS;
}

/* ── Classification logic ──────────────────────────────────────────────── */

void classify_delegate_exit(const dl_exit_metrics_t *m, dl_classification_t *out)
{
   memset(out, 0, sizeof(*out));

   if (!m->success)
   {
      /* Failure path — determine specific mode */
      if (m->write_enforce_fired && !m->had_writes)
      {
         /* Stall with no writes */
         out->failure_mode = DL_MODE_STALL_NO_WRITES;
         if (m->turns >= 14)
            out->confidence = 0.9;
         else
            out->confidence = 0.6;
         snprintf(out->lesson, sizeof(out->lesson),
                  "Delegate stalled with zero writes after %d turns in role '%s'. "
                  "Ensure the task is actionable and files are writable. "
                  "Break large tasks into smaller sub-delegations.",
                  m->turns, m->role ? m->role : "unknown");
         snprintf(out->evidence, sizeof(out->evidence),
                  "{\"turns\":%d,\"tool_calls\":%d,\"had_writes\":0,\"write_enforce\":1}", m->turns,
                  m->tool_calls);
      }
      else if (m->write_enforce_fired && m->had_writes)
      {
         /* Stall but eventually wrote */
         out->failure_mode = DL_MODE_STALL_SLOW_WRITES;
         out->confidence = 0.6;
         snprintf(out->lesson, sizeof(out->lesson),
                  "Delegate was slow to produce writes (%d turns) in role '%s'. "
                  "Consider tightening the prompt to require early writes.",
                  m->turns, m->role ? m->role : "unknown");
         snprintf(out->evidence, sizeof(out->evidence),
                  "{\"turns\":%d,\"tool_calls\":%d,\"had_writes\":1,\"write_enforce\":1}", m->turns,
                  m->tool_calls);
      }
      else if (m->max_turns_limit > 0 && m->turns >= m->max_turns_limit)
      {
         /* Max turns exhausted */
         out->failure_mode = DL_MODE_MAX_TURNS;
         if (!m->had_writes)
            out->confidence = 0.85;
         else
            out->confidence = 0.6;
         snprintf(out->lesson, sizeof(out->lesson),
                  "Delegate hit max-turns limit (%d/%d) in role '%s'. "
                  "Task may be too complex for the turn budget. "
                  "Split into smaller delegations or increase max_turns.",
                  m->turns, m->max_turns_limit, m->role ? m->role : "unknown");
         snprintf(out->evidence, sizeof(out->evidence),
                  "{\"turns\":%d,\"max_turns\":%d,\"had_writes\":%d}", m->turns, m->max_turns_limit,
                  m->had_writes);
      }
      else if (m->error && strstr(m->error, "drift") != NULL)
      {
         /* Drift detected */
         if (strstr(m->error, "pre-flight") != NULL || strstr(m->error, "preflight") != NULL)
         {
            out->failure_mode = DL_MODE_DRIFT_PREFLIGHT;
         }
         else
         {
            out->failure_mode = DL_MODE_DRIFT_BRANCH;
         }
         out->confidence = 0.6;
         snprintf(out->lesson, sizeof(out->lesson),
                  "Delegate drifted from expected state in role '%s'. "
                  "Ground the prompt with current file contents or diff evidence.",
                  m->role ? m->role : "unknown");
         snprintf(out->evidence, sizeof(out->evidence), "{\"error\":\"%s\",\"turns\":%d}", m->error,
                  m->turns);
      }
      else if (m->error && strstr(m->error, "cancel") != NULL)
      {
         out->failure_mode = DL_MODE_CANCELLED;
         out->confidence = 0.6;
         snprintf(out->lesson, sizeof(out->lesson),
                  "Delegate was cancelled in role '%s' after %d turns. "
                  "Ensure the task can complete within the timeout.",
                  m->role ? m->role : "unknown", m->turns);
         snprintf(out->evidence, sizeof(out->evidence), "{\"error\":\"%s\",\"turns\":%d}", m->error,
                  m->turns);
      }
      else
      {
         /* Generic failure — classify as max-turns if turns were high, else unknown */
         out->failure_mode = DL_MODE_MAX_TURNS;
         out->confidence = 0.6;
         snprintf(out->lesson, sizeof(out->lesson),
                  "Delegate failed in role '%s' after %d turns: %s", m->role ? m->role : "unknown",
                  m->turns, m->error ? m->error : "unknown error");
         snprintf(out->evidence, sizeof(out->evidence),
                  "{\"error\":\"%s\",\"turns\":%d,\"tool_calls\":%d}",
                  m->error ? m->error : "unknown", m->turns, m->tool_calls);
      }
   }
   else
   {
      /* Success */
      out->failure_mode = DL_MODE_SUCCESS;
      out->confidence = 0.6;
      snprintf(out->lesson, sizeof(out->lesson),
               "Delegate succeeded in role '%s' in %d turns with %d tool calls. "
               "This pattern worked — reuse similar prompt structure.",
               m->role ? m->role : "unknown", m->turns, m->tool_calls);
      snprintf(out->evidence, sizeof(out->evidence),
               "{\"turns\":%d,\"tool_calls\":%d,\"had_writes\":%d}", m->turns, m->tool_calls,
               m->had_writes);
   }
}

/* ── DB1 operations ────────────────────────────────────────────────────── */

/* Schema is in src/db1/schema.sql; applied at db1_init time. */

int delegate_learning_evict_if_needed(void)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   /* Count rows */
   int row_count = 0;
   {
      sqlite3_stmt *stmt;
      if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM delegate_learnings;", -1, &stmt, NULL) ==
          SQLITE_OK)
      {
         if (sqlite3_step(stmt) == SQLITE_ROW)
            row_count = sqlite3_column_int(stmt, 0);
         sqlite3_finalize(stmt);
      }
   }

   if (row_count <= DL_TABLE_CAP)
      return 0;

   /* Evict oldest reviewed/rejected first */
   {
      sqlite3_stmt *stmt;
      const char *sql = "DELETE FROM delegate_learnings WHERE id IN ("
                        " SELECT id FROM delegate_learnings"
                        " WHERE review_status IN ('reviewed','rejected')"
                        " ORDER BY created_at ASC LIMIT ?);";
      if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK)
      {
         sqlite3_bind_int(stmt, 1, row_count - DL_TABLE_CAP);
         sqlite3_step(stmt);
         sqlite3_finalize(stmt);
      }
   }

   /* Re-check and evict oldest pending if still over cap */
   {
      sqlite3_stmt *stmt;
      if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM delegate_learnings;", -1, &stmt, NULL) ==
          SQLITE_OK)
      {
         if (sqlite3_step(stmt) == SQLITE_ROW)
            row_count = sqlite3_column_int(stmt, 0);
         sqlite3_finalize(stmt);
      }
   }
   if (row_count > DL_TABLE_CAP)
   {
      sqlite3_stmt *stmt;
      const char *sql = "DELETE FROM delegate_learnings WHERE id IN ("
                        " SELECT id FROM delegate_learnings"
                        " WHERE review_status = 'pending'"
                        " ORDER BY created_at ASC LIMIT ?);";
      if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK)
      {
         sqlite3_bind_int(stmt, 1, row_count - DL_TABLE_CAP);
         sqlite3_step(stmt);
         sqlite3_finalize(stmt);
      }
   }

   return 0;
}

int delegate_learning_record(const char *session_id, const char *role,
                             dl_failure_mode_t failure_mode, const char *lesson,
                             const char *evidence_json, double confidence)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   delegate_learning_evict_if_needed();

   const char *sql = "INSERT INTO delegate_learnings"
                     " (session_id, role, failure_mode, lesson, evidence_json,"
                     "  confidence, auto_applied, review_status)"
                     " VALUES (?,?,?,?,?,?,1,'pending');";

   sqlite3_stmt *stmt;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, session_id ? session_id : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 2, role ? role : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, dl_failure_mode_to_string(failure_mode), -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 4, lesson ? lesson : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 5, evidence_json ? evidence_json : "{}", -1, SQLITE_STATIC);
   sqlite3_bind_double(stmt, 6, confidence);

   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return 0;
}

/* ── Prompt injection ──────────────────────────────────────────────────── */

char *delegate_learning_inject_prompt(const char *role, const char *system_prompt, int top_n)
{
   if (!role || !system_prompt)
      return NULL;

   if (top_n <= 0)
      top_n = 3;

   /* Query top-N highest-confidence learnings for this role */
   char sql[512];
   snprintf(sql, sizeof(sql),
            "SELECT lesson, confidence FROM delegate_learnings "
            "WHERE role = ? AND auto_applied = 1 "
            "ORDER BY confidence DESC, created_at DESC "
            "LIMIT %d;",
            top_n);

   sqlite3 *db = db1_conn();
   if (!db)
      return NULL;

   sqlite3_stmt *stmt;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return NULL;

   sqlite3_bind_text(stmt, 1, role, -1, SQLITE_STATIC);

   /* Collect lessons */
   char *lessons[10];
   int lesson_count = 0;

   while (sqlite3_step(stmt) == SQLITE_ROW && lesson_count < 10)
   {
      const char *lesson_text = (const char *)sqlite3_column_text(stmt, 0);
      if (lesson_text && lesson_text[0])
      {
         lessons[lesson_count] = strdup(lesson_text);
         lesson_count++;
      }
   }
   sqlite3_finalize(stmt);

   if (lesson_count == 0)
      return NULL;

   /* Build injection block */
   char *injection = NULL;
   {
      size_t total = 0;
      total += 128; /* header */
      for (int i = 0; i < lesson_count; i++)
         total += strlen(lessons[i]) + 32;

      injection = malloc(total);
      if (!injection)
      {
         for (int i = 0; i < lesson_count; i++)
            free(lessons[i]);
         return NULL;
      }

      int offset = 0;
      offset += snprintf(injection + offset, total - offset,
                         "\n## Past Learnings for Role '%s' (auto-applied)\n\n", role);
      for (int i = 0; i < lesson_count; i++)
      {
         offset += snprintf(injection + offset, total - offset, "- %s\n", lessons[i]);
      }
      offset += snprintf(injection + offset, total - offset,
                         "\nUse these learnings to guide your approach.\n");

      for (int i = 0; i < lesson_count; i++)
         free(lessons[i]);
   }

   /* Prepend injection to system prompt */
   size_t inj_len = strlen(injection);
   size_t sys_len = strlen(system_prompt);
   char *combined = malloc(inj_len + sys_len + 1);
   if (combined)
   {
      memcpy(combined, injection, inj_len);
      memcpy(combined + inj_len, system_prompt, sys_len + 1);
   }
   free(injection);

   return combined;
}
