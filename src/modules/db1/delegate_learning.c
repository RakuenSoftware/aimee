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

static int delegate_learning_evict_if_needed(void)
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

int db1_delegate_learning_record(const char *session_id, const char *role,
                                 const char *failure_mode, const char *lesson,
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
   sqlite3_bind_text(stmt, 3, failure_mode ? failure_mode : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, lesson ? lesson : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 5, evidence_json ? evidence_json : "{}", -1, SQLITE_STATIC);
   sqlite3_bind_double(stmt, 6, confidence);

   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return 0;
}

/* ── Prompt injection ──────────────────────────────────────────────────── */

char *db1_delegate_learning_inject_prompt(const char *role, const char *system_prompt, int top_n)
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
