/* db2/anti_patterns.c: anti-pattern catalog — Postgres via libpq. */

#include "anti_patterns.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define AP_ERRBUF 256

/* Word-boundary predicate for anti-pattern phrase matching. A word char is
 * alphanumeric or underscore; everything else (whitespace, slash, dash,
 * punctuation, NUL) counts as a boundary. */
static int ap_is_word_char(unsigned char c)
{
   return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/* Copy src into dst, lowercasing and collapsing any run of whitespace to a
 * single space. Leading/trailing whitespace is stripped. */
static void ap_normalize(const char *src, char *dst, size_t dst_len)
{
   if (dst_len == 0)
      return;
   size_t o = 0;
   int in_space = 1; /* treat leading space as already-collapsed */
   for (size_t i = 0; src && src[i] && o + 1 < dst_len; i++)
   {
      unsigned char c = (unsigned char)src[i];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
      {
         if (!in_space)
         {
            dst[o++] = ' ';
            in_space = 1;
         }
      }
      else
      {
         dst[o++] = (char)tolower(c);
         in_space = 0;
      }
   }
   while (o > 0 && dst[o - 1] == ' ')
      o--;
   dst[o] = '\0';
}

/* Returns 1 iff target contains pattern as a word-bounded phrase. Both
 * inputs must already be normalized (lowercased, single-spaced, trimmed).
 * An empty pattern never matches — prevents zero-length rows from matching
 * everything. */
static int ap_phrase_match(const char *pattern, const char *target)
{
   if (!pattern || !*pattern || !target)
      return 0;
   size_t plen = strlen(pattern);
   size_t tlen = strlen(target);
   if (plen > tlen)
      return 0;

   for (size_t i = 0; i + plen <= tlen; i++)
   {
      if (memcmp(target + i, pattern, plen) != 0)
         continue;
      if (i > 0 && ap_is_word_char((unsigned char)target[i - 1]))
         continue;
      unsigned char next = (unsigned char)target[i + plen];
      if (next != '\0' && ap_is_word_char(next))
         continue;
      return 1;
   }
   return 0;
}

int db2_anti_pattern_insert(const char *pattern, const char *description, const char *source,
                            const char *source_ref, double confidence, anti_pattern_t *out)
{
   void *conn = db2_conn();
   if (!conn || !pattern)
      return -1;

   char err[AP_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO anti_patterns (pattern, description, source, source_ref, "
                        "confidence) VALUES (?1, ?2, ?3, ?4, ?5) RETURNING id",
                        err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", pattern);
   aimee_pg_bind_text(st, "?2", description ? description : "");
   aimee_pg_bind_text(st, "?3", source ? source : "");
   aimee_pg_bind_text(st, "?4", source_ref ? source_ref : "");
   aimee_pg_bind_double(st, "?5", confidence);

   int64_t new_id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      new_id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   if (new_id < 0)
      return -1;

   if (out)
   {
      memset(out, 0, sizeof(*out));
      out->id = new_id;
      snprintf(out->pattern, sizeof(out->pattern), "%s", pattern);
      snprintf(out->description, sizeof(out->description), "%s", description ? description : "");
      snprintf(out->source, sizeof(out->source), "%s", source ? source : "");
      snprintf(out->source_ref, sizeof(out->source_ref), "%s", source_ref ? source_ref : "");
      out->hit_count = 0;
      out->confidence = confidence;
   }
   return 0;
}

int db2_anti_pattern_list(anti_pattern_t *out, int max)
{
   void *conn = db2_conn();
   if (!conn || !out)
      return 0;

   char err[AP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT id, pattern, description, source, source_ref, hit_count, confidence "
       "FROM anti_patterns ORDER BY hit_count DESC, confidence DESC",
       err, sizeof(err));
   if (!st)
      return 0;

   int count = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && count < max)
   {
      out[count].id = aimee_pg_column_int64(st, 0);
      db2_copy_col_text(out[count].pattern, sizeof(out[count].pattern), st, 1);
      db2_copy_col_text(out[count].description, sizeof(out[count].description), st, 2);
      db2_copy_col_text(out[count].source, sizeof(out[count].source), st, 3);
      db2_copy_col_text(out[count].source_ref, sizeof(out[count].source_ref), st, 4);
      out[count].hit_count = aimee_pg_column_int(st, 5);
      out[count].confidence = aimee_pg_column_double(st, 6);
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}

int db2_anti_pattern_check(const char *file_path, const char *command, anti_pattern_t *out, int max)
{
   void *conn = db2_conn();
   if (!conn || !out)
      return 0;

   /* Build raw target, then normalize once. snprintf returns the would-be
    * length, so a long file_path can run rpos past the buffer; clamp after each
    * append so the next (sizeof - rpos) cannot wrap to a huge size_t and so the
    * final raw[rpos] = '\0' stays in bounds. */
   char raw[4096];
   int rpos = 0;
   if (file_path)
      rpos += snprintf(raw + rpos, sizeof(raw) - (size_t)rpos, "%s ", file_path);
   if (rpos > (int)sizeof(raw) - 1)
      rpos = (int)sizeof(raw) - 1;
   if (command)
      rpos += snprintf(raw + rpos, sizeof(raw) - (size_t)rpos, "%s", command);
   if (rpos > (int)sizeof(raw) - 1)
      rpos = (int)sizeof(raw) - 1;
   raw[rpos] = '\0';

   char target[4096];
   ap_normalize(raw, target, sizeof(target));

   char err[AP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT id, pattern, description, source, source_ref, hit_count, confidence "
       "FROM anti_patterns ORDER BY confidence DESC",
       err, sizeof(err));
   if (!st)
      return 0;

   int count = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && count < max)
   {
      const char *pattern = aimee_pg_column_text(st, 1);
      if (!pattern || !*pattern)
         continue;

      char pat_norm[512];
      ap_normalize(pattern, pat_norm, sizeof(pat_norm));
      if (!pat_norm[0])
         continue;

      if (!ap_phrase_match(pat_norm, target))
         continue;

      out[count].id = aimee_pg_column_int64(st, 0);
      snprintf(out[count].pattern, sizeof(out[count].pattern), "%s", pattern);
      db2_copy_col_text(out[count].description, sizeof(out[count].description), st, 2);
      db2_copy_col_text(out[count].source, sizeof(out[count].source), st, 3);
      db2_copy_col_text(out[count].source_ref, sizeof(out[count].source_ref), st, 4);
      out[count].hit_count = aimee_pg_column_int(st, 5);
      out[count].confidence = aimee_pg_column_double(st, 6);
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}

int db2_anti_pattern_exists_exact(const char *pattern)
{
   void *conn = db2_conn();
   if (!conn || !pattern)
      return 0;

   char err[AP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT COUNT(*) FROM anti_patterns WHERE pattern = ?1", err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", pattern);

   int exists = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      exists = aimee_pg_column_int(st, 0) > 0;
   aimee_pg_finalize(st);
   return exists;
}

int db2_anti_pattern_exists_by_source_ref(const char *source_ref)
{
   void *conn = db2_conn();
   if (!conn || !source_ref)
      return 0;

   char err[AP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT COUNT(*) FROM anti_patterns WHERE source_ref = ?1", err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", source_ref);

   int exists = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      exists = aimee_pg_column_int(st, 0) > 0;
   aimee_pg_finalize(st);
   return exists;
}

int db2_anti_pattern_bump(int64_t id)
{
   void *conn = db2_conn();
   if (!conn || id <= 0)
      return -1;

   char err[AP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "UPDATE anti_patterns SET hit_count = hit_count + 1 WHERE id = ?1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", id);
   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE) ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

int db2_anti_pattern_delete(int64_t id)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[AP_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "DELETE FROM anti_patterns WHERE id = ?1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", id);
   aimee_pg_step_t step_rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   if (step_rc != AIMEE_PG_DONE)
      return -1;
   return changes > 0 ? 0 : -1;
}

int db2_anti_pattern_list_hot(int threshold, anti_pattern_t *out, int max)
{
   void *conn = db2_conn();
   if (!conn || !out || threshold <= 0)
      return 0;

   char err[AP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT id, pattern, description, source, source_ref, hit_count, confidence "
       "FROM anti_patterns WHERE hit_count >= ?1 ORDER BY hit_count DESC",
       err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "?1", threshold);

   int count = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && count < max)
   {
      out[count].id = aimee_pg_column_int64(st, 0);
      db2_copy_col_text(out[count].pattern, sizeof(out[count].pattern), st, 1);
      db2_copy_col_text(out[count].description, sizeof(out[count].description), st, 2);
      db2_copy_col_text(out[count].source, sizeof(out[count].source), st, 3);
      db2_copy_col_text(out[count].source_ref, sizeof(out[count].source_ref), st, 4);
      out[count].hit_count = aimee_pg_column_int(st, 5);
      out[count].confidence = aimee_pg_column_double(st, 6);
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}
