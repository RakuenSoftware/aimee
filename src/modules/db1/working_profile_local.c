/* db1/working_profile_local.c: per-machine learned operator preferences. */

#include "working_profile_local.h"
#include "db1_internal.h"

#include <ctype.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB1_WORKING_PROFILE_KEY_LEN          1024
#define WORKING_PROFILE_CALIBRATION_TAU_AUTO 0.80

extern int db2_calibration_profile_read(const char *target_surface, const char *kind,
                                        const char *scope_kind, const char *scope_id, char *buf,
                                        size_t len) __attribute__((weak));
extern int db2_calibration_threshold_from_profile_json(const char *payload_json,
                                                       double static_threshold,
                                                       double *threshold_out) __attribute__((weak));

static int hex_digit(int ch)
{
   if (ch >= '0' && ch <= '9')
      return ch - '0';
   if (ch >= 'a' && ch <= 'f')
      return ch - 'a' + 10;
   if (ch >= 'A' && ch <= 'F')
      return ch - 'A' + 10;
   return -1;
}

static size_t encode_component(char *dst, size_t cap, const char *src)
{
   size_t n = 0;

   if (!dst || cap == 0)
      return 0;
   dst[0] = '\0';
   if (!src)
      return 0;

   while (*src)
   {
      unsigned char ch = (unsigned char)*src++;
      if (isalnum(ch) || ch == '_' || ch == '-' || ch == '.')
      {
         if (n + 1 >= cap)
            return 0;
         dst[n++] = (char)ch;
      }
      else
      {
         if (n + 3 >= cap)
            return 0;
         snprintf(dst + n, cap - n, "%%%02X", ch);
         n += 3;
      }
   }

   dst[n] = '\0';
   return n;
}

static int decode_component(char *dst, size_t cap, const char *src, size_t len)
{
   size_t n = 0;

   if (!dst || cap == 0)
      return -1;
   dst[0] = '\0';
   if (!src)
      return -1;

   for (size_t i = 0; i < len; i++)
   {
      unsigned char ch = (unsigned char)src[i];
      if (ch == '%')
      {
         int hi;
         int lo;

         if (i + 2 >= len)
            return -1;
         hi = hex_digit((unsigned char)src[i + 1]);
         lo = hex_digit((unsigned char)src[i + 2]);
         if (hi < 0 || lo < 0)
            return -1;
         ch = (unsigned char)((hi << 4) | lo);
         i += 2;
      }
      if (n + 1 >= cap)
         return -1;
      dst[n++] = (char)ch;
   }

   dst[n] = '\0';
   return 0;
}

static int compose_key(char *dst, size_t cap, const char *field, const char *value)
{
   char enc_field[DB1_WORKING_PROFILE_KEY_LEN];
   char enc_value[DB1_WORKING_PROFILE_KEY_LEN];

   if (!dst || !field || !field[0] || !value || !value[0])
      return -1;
   if (encode_component(enc_field, sizeof(enc_field), field) == 0)
      return -1;
   if (encode_component(enc_value, sizeof(enc_value), value) == 0)
      return -1;
   if (snprintf(dst, cap, "%s:%s", enc_field, enc_value) >= (int)cap)
      return -1;
   return 0;
}

static int compose_field_glob(char *dst, size_t cap, const char *field)
{
   char enc_field[DB1_WORKING_PROFILE_KEY_LEN];

   if (!dst || !field || !field[0])
      return -1;
   if (encode_component(enc_field, sizeof(enc_field), field) == 0)
      return -1;
   if (snprintf(dst, cap, "%s:*", enc_field) >= (int)cap)
      return -1;
   return 0;
}

static int parse_key(const char *key, char *field, size_t field_cap, char *value, size_t value_cap)
{
   const char *sep;

   if (!key || !field || !value)
      return -1;
   sep = strchr(key, ':');
   if (!sep || sep == key || !sep[1])
      return -1;
   if (decode_component(field, field_cap, key, (size_t)(sep - key)) != 0)
      return -1;
   if (decode_component(value, value_cap, sep + 1, strlen(sep + 1)) != 0)
      return -1;
   return 0;
}

static double parse_confidence_text(const unsigned char *text)
{
   char *end = NULL;
   double value;

   if (!text || !text[0])
      return 0.0;
   value = strtod((const char *)text, &end);
   return end != (const char *)text ? value : 0.0;
}

static int candidate_stats(sqlite3 *db, const char *key, const char *value, int *count_out,
                           double *avg_out)
{
   sqlite3_stmt *stmt = NULL;
   int count = 0;
   double sum = 0.0;
   static const char *sql = "SELECT payload_json FROM working_profile_observations_local"
                            " WHERE working_profile_key = ? AND signal = ?";

   if (!db || !key || !value || !count_out || !avg_out)
      return -1;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      sum += parse_confidence_text(sqlite3_column_text(stmt, 0));
      count++;
   }
   sqlite3_finalize(stmt);

   *count_out = count;
   *avg_out = count > 0 ? (sum / (double)count) : 0.0;
   return 0;
}

static int delete_field_rows(sqlite3 *db, const char *field_glob, const char *table)
{
   sqlite3_stmt *stmt = NULL;
   char sql[256];

   if (!db || !field_glob || !table || !table[0])
      return -1;
   snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE working_profile_key GLOB ?", table);
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, field_glob, -1, SQLITE_TRANSIENT);
   if (sqlite3_step(stmt) != SQLITE_DONE)
   {
      sqlite3_finalize(stmt);
      return -1;
   }
   sqlite3_finalize(stmt);
   return 0;
}

static int load_state_from_stmt(sqlite3_stmt *stmt, db1_working_profile_local_state_t *out)
{
   const unsigned char *key = sqlite3_column_text(stmt, 0);

   if (!key || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   if (parse_key((const char *)key, out->field, sizeof(out->field), out->value,
                 sizeof(out->value)) != 0)
      return -1;
   out->score = sqlite3_column_double(stmt, 1);
   out->observation_count = sqlite3_column_int(stmt, 2);
   db1_copy_col_text(out->updated_at, sizeof(out->updated_at), stmt, 3);
   return 0;
}

int db1_working_profile_local_observe(const char *field, const char *value, double confidence,
                                      const char *session_id, int threshold)
{
   sqlite3 *db = db1_conn();
   sqlite3_stmt *stmt = NULL;
   db1_working_profile_local_state_t existing;
   char key[DB1_WORKING_PROFILE_KEY_LEN];
   char field_glob[DB1_WORKING_PROFILE_KEY_LEN];
   char confidence_buf[32];
   int count = 0;
   double avg = 0.0;
   int have_existing;
   static const char *ins_obs_sql =
       "INSERT INTO working_profile_observations_local"
       " (working_profile_key, session_id, signal, payload_json, created_at)"
       " VALUES (?, ?, ?, ?, datetime('now'))";
   static const char *ins_state_sql =
       "INSERT INTO working_profile_state_local"
       " (working_profile_key, score, observation_count, last_observation_at, updated_at)"
       " VALUES (?, ?, ?, datetime('now'), datetime('now'))";

   if (!db || !field || !field[0] || !value || !value[0])
      return -1;
   if (threshold <= 0)
      threshold = 3;
   if (confidence < 0.0)
      confidence = 0.0;
   if (confidence > 1.0)
      confidence = 1.0;
   if (compose_key(key, sizeof(key), field, value) != 0)
      return -1;
   if (compose_field_glob(field_glob, sizeof(field_glob), field) != 0)
      return -1;

   if (sqlite3_prepare_v2(db, ins_obs_sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, session_id ? session_id : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, value, -1, SQLITE_TRANSIENT);
   snprintf(confidence_buf, sizeof(confidence_buf), "%.17g", confidence);
   sqlite3_bind_text(stmt, 4, confidence_buf, -1, SQLITE_TRANSIENT);
   if (sqlite3_step(stmt) != SQLITE_DONE)
   {
      sqlite3_finalize(stmt);
      return -1;
   }
   sqlite3_finalize(stmt);

   if (candidate_stats(db, key, value, &count, &avg) != 0)
      return -1;
   if (count < threshold)
      return 0;

   char cal_buf[4096];
   double calibrated_score = WORKING_PROFILE_CALIBRATION_TAU_AUTO;
   if (db2_calibration_profile_read && db2_calibration_threshold_from_profile_json &&
       db2_calibration_profile_read("working_profile", "field", "global", "", cal_buf,
                                    sizeof(cal_buf)) == 0 &&
       db2_calibration_threshold_from_profile_json(cal_buf, WORKING_PROFILE_CALIBRATION_TAU_AUTO,
                                                   &calibrated_score) == 0 &&
       avg < calibrated_score)
      return 0;

   have_existing = db1_working_profile_local_get(field, &existing) == 0;
   if (have_existing && strcmp(existing.value, value) != 0 && existing.score > avg)
      return 0;

   if (delete_field_rows(db, field_glob, "working_profile_state_local") != 0)
      return -1;
   if (sqlite3_prepare_v2(db, ins_state_sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
   sqlite3_bind_double(stmt, 2, avg);
   sqlite3_bind_int(stmt, 3, count);
   if (sqlite3_step(stmt) != SQLITE_DONE)
   {
      sqlite3_finalize(stmt);
      return -1;
   }
   sqlite3_finalize(stmt);
   return 1;
}

int db1_working_profile_local_list(db1_working_profile_local_state_t *out, int max)
{
   sqlite3 *db = db1_conn();
   sqlite3_stmt *stmt = NULL;
   int n = 0;
   static const char *sql = "SELECT working_profile_key, score, observation_count, updated_at"
                            " FROM working_profile_state_local"
                            " ORDER BY score DESC, updated_at DESC LIMIT ?";

   if (!out || max <= 0)
      return 0;
   if (!db)
      return -1;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_int(stmt, 1, max);
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      if (load_state_from_stmt(stmt, &out[n]) == 0)
         n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_working_profile_local_get(const char *field, db1_working_profile_local_state_t *out)
{
   sqlite3 *db = db1_conn();
   sqlite3_stmt *stmt = NULL;
   char field_glob[DB1_WORKING_PROFILE_KEY_LEN];
   static const char *sql = "SELECT working_profile_key, score, observation_count, updated_at"
                            " FROM working_profile_state_local"
                            " WHERE working_profile_key GLOB ?"
                            " ORDER BY updated_at DESC LIMIT 1";

   if (!field || !field[0] || !out)
      return -1;
   if (!db)
      return -1;
   if (compose_field_glob(field_glob, sizeof(field_glob), field) != 0)
      return -1;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, field_glob, -1, SQLITE_TRANSIENT);
   if (sqlite3_step(stmt) != SQLITE_ROW)
   {
      sqlite3_finalize(stmt);
      return 1;
   }
   if (load_state_from_stmt(stmt, out) != 0)
   {
      sqlite3_finalize(stmt);
      return -1;
   }
   sqlite3_finalize(stmt);
   return 0;
}

int db1_working_profile_local_reset_field(const char *field)
{
   sqlite3 *db = db1_conn();
   char field_glob[DB1_WORKING_PROFILE_KEY_LEN];

   if (!field || !field[0])
      return -1;
   if (!db)
      return -1;
   if (compose_field_glob(field_glob, sizeof(field_glob), field) != 0)
      return -1;
   if (delete_field_rows(db, field_glob, "working_profile_state_local") != 0)
      return -1;
   if (delete_field_rows(db, field_glob, "working_profile_observations_local") != 0)
      return -1;
   return 0;
}
