/* db2/corpus_structural.c: deterministic corpus structural analysis. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "corpus_structural.h"
#include "util.h"
#include "artifacts.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "aimee.h"
#include "cJSON.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CS_ERRBUF 256

typedef struct
{
   int64_t id;
   char filename[512];
   char content_hash[128];
   char normalized_text[16384];
} corpus_doc_text_t;

static int ends_with_ci(const char *s, const char *suffix)
{
   if (!s || !suffix)
      return 0;
   size_t sl = strlen(s);
   size_t su = strlen(suffix);
   if (su > sl)
      return 0;
   return strcasecmp(s + sl - su, suffix) == 0;
}

static uint64_t fnv1a_bytes(const char *s, size_t n)
{
   uint64_t h = 1469598103934665603ULL;
   for (size_t i = 0; i < n; i++)
   {
      h ^= (unsigned char)s[i];
      h *= 1099511628211ULL;
   }
   return h;
}

static void hash_span(const char *text, int64_t start, int64_t end, char *out, size_t cap)
{
   if (!text || start < 0 || end < start)
   {
      db2_copy_text(out, cap, "");
      return;
   }
   uint64_t h = fnv1a_bytes(text + start, (size_t)(end - start));
   snprintf(out, cap, "%016llx", (unsigned long long)h);
}

static int corpus_doc_load(int64_t doc_id, corpus_doc_text_t *out)
{
   void *conn = db2_conn();
   if (!conn || !out || doc_id <= 0)
      return -1;

   char err[CS_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT id, filename, content_hash, normalized_text FROM docs WHERE id = ?1", err,
       sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", doc_id);

   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      return -1;
   }

   memset(out, 0, sizeof(*out));
   out->id = aimee_pg_column_int64(st, 0);
   db2_copy_text(out->filename, sizeof(out->filename), aimee_pg_column_text(st, 1));
   db2_copy_text(out->content_hash, sizeof(out->content_hash), aimee_pg_column_text(st, 2));
   db2_copy_text(out->normalized_text, sizeof(out->normalized_text), aimee_pg_column_text(st, 3));
   aimee_pg_finalize(st);
   return 0;
}

const char *db2_corpus_classify_type(const char *filename, const char *normalized_text,
                                     double *confidence_out)
{
   const char *fn = filename ? filename : "";
   const char *txt = normalized_text ? normalized_text : "";
   double confidence = 0.55;
   const char *kind = "other";

   if (ends_with_ci(fn, ".c") || ends_with_ci(fn, ".h") || ends_with_ci(fn, ".cpp") ||
       ends_with_ci(fn, ".cc") || ends_with_ci(fn, ".cxx") || ends_with_ci(fn, ".hpp") ||
       ends_with_ci(fn, ".rs") || ends_with_ci(fn, ".go") || ends_with_ci(fn, ".py") ||
       ends_with_ci(fn, ".ts") || ends_with_ci(fn, ".js") || ends_with_ci(fn, ".java") ||
       ends_with_ci(fn, ".cs") || ends_with_ci(fn, ".rb") || ends_with_ci(fn, ".sh") ||
       str_contains_ci(txt, "```c\n") || str_contains_ci(txt, "```python\n") ||
       str_contains_ci(txt, "```rust\n") || str_contains_ci(txt, "```go\n"))
   {
      kind = "code";
      confidence = 1.0;
   }
   else if (str_contains_ci(fn, "architecture") || str_contains_ci(fn, "/adr/") ||
            str_contains_ci(fn, "adr-") || str_contains_ci(txt, "# architecture") ||
            str_contains_ci(txt, "architecture charter"))
   {
      kind = "architecture";
      confidence = 0.9;
   }
   else if (str_contains_ci(fn, "proposal") || str_contains_ci(fn, "design") ||
            str_contains_ci(txt, "# proposal:") || str_contains_ci(txt, "## approach"))
   {
      kind = "design";
      confidence = 0.82;
   }
   else if (str_contains_ci(fn, "spec") || str_contains_ci(fn, "requirements") ||
            str_contains_ci(txt, "acceptance criteria") || str_contains_ci(txt, "must "))
   {
      kind = "spec";
      confidence = 0.78;
   }
   else if (str_contains_ci(fn, "implementation") || str_contains_ci(fn, "runbook") ||
            str_contains_ci(txt, "rollout") || str_contains_ci(txt, "operational"))
   {
      kind = "implementation";
      confidence = 0.72;
   }

   if (confidence_out)
      *confidence_out = confidence;
   return kind;
}

static char *json_print_take(cJSON *root)
{
   char *s = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   return s;
}

int db2_corpus_classify_doc(int64_t doc_id, const char *operator_id)
{
   corpus_doc_text_t doc;
   if (corpus_doc_load(doc_id, &doc) != 0)
      return -1;

   double conf = 0.0;
   const char *kind = db2_corpus_classify_type(doc.filename, doc.normalized_text, &conf);

   void *conn = db2_conn();
   if (!conn)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   char err[CS_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "UPDATE docs SET doc_type = ?1, doc_type_confidence = ?2, updated_at = ?3 WHERE id = ?4",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", kind);
   aimee_pg_bind_double(st, "?2", conf);
   aimee_pg_bind_text(st, "?3", ts);
   aimee_pg_bind_int64(st, "?4", doc_id);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   if (rc != AIMEE_PG_DONE && rc != AIMEE_PG_ROW)
      return -1;

   char artifact_id[37], audit_id[37];
   db2_artifact_gen_id(artifact_id, sizeof(artifact_id));
   db2_artifact_gen_id(audit_id, sizeof(audit_id));

   cJSON *payload = cJSON_CreateObject();
   cJSON_AddNumberToObject(payload, "doc_id", (double)doc_id);
   cJSON_AddStringToObject(payload, "filename", doc.filename);
   cJSON_AddStringToObject(payload, "doc_type", kind);
   cJSON_AddNumberToObject(payload, "confidence", conf);
   char *payload_json = json_print_take(payload);

   int arc = db2_artifact_write(artifact_id, "doc_classification", "committed", "global", "",
                                operator_id ? operator_id : "", conf, payload_json);
   free(payload_json);
   if (arc != 0)
      return -1;

   char target[64];
   snprintf(target, sizeof(target), "%lld", (long long)doc_id);
   cJSON *after = cJSON_CreateObject();
   cJSON_AddStringToObject(after, "doc_type", kind);
   cJSON_AddNumberToObject(after, "doc_type_confidence", conf);
   char *after_json = json_print_take(after);
   int aud =
       db2_audit_event_write(audit_id, artifact_id, "docs", target, operator_id ? operator_id : "",
                             "global", "", conf, 0, NULL, after_json);
   free(after_json);
   return aud;
}

static int delete_sections_for_doc(int64_t doc_id)
{
   void *conn = db2_conn();
   char err[CS_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "DELETE FROM document_sections WHERE doc_id = ?1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", doc_id);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

static int update_section_end(int64_t section_id, int64_t span_start, int64_t span_end,
                              const char *text)
{
   if (section_id <= 0)
      return 0;
   char h[65];
   hash_span(text, span_start, span_end, h, sizeof(h));
   void *conn = db2_conn();
   char err[CS_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "UPDATE document_sections SET span_end = ?1, content_hash = ?2 WHERE id = ?3", err,
       sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", span_end);
   aimee_pg_bind_text(st, "?2", h);
   aimee_pg_bind_int64(st, "?3", section_id);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

static int64_t insert_section(int64_t doc_id, int64_t parent_id, int64_t ordinal, int depth,
                              const char *heading, const char *heading_path, int64_t span_start,
                              int64_t span_end, const char *hash)
{
   void *conn = db2_conn();
   char err[CS_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "INSERT INTO document_sections"
       " (doc_id, parent_id, ordinal, depth, heading, heading_path, span_start, span_end,"
       "  content_hash)"
       " VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9)"
       " RETURNING id",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", doc_id);
   if (parent_id > 0)
      aimee_pg_bind_int64(st, "?2", parent_id);
   else
      aimee_pg_bind_null(st, "?2");
   aimee_pg_bind_int64(st, "?3", ordinal);
   aimee_pg_bind_int(st, "?4", depth);
   aimee_pg_bind_text(st, "?5", heading);
   aimee_pg_bind_text(st, "?6", heading_path);
   aimee_pg_bind_int64(st, "?7", span_start);
   aimee_pg_bind_int64(st, "?8", span_end);
   aimee_pg_bind_text(st, "?9", hash ? hash : "");

   int64_t id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

static int parse_heading(const char *line, int *depth_out, char *heading, size_t heading_cap)
{
   int depth = 0;
   while (line[depth] == '#' && depth < 6)
      depth++;
   if (depth == 0 || line[depth] != ' ')
      return 0;
   const char *p = line + depth + 1;
   while (*p == ' ' || *p == '\t')
      p++;
   size_t n = strlen(p);
   while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r' || isspace((unsigned char)p[n - 1])))
      n--;
   if (n == 0)
      return 0;
   if (n >= heading_cap)
      n = heading_cap - 1;
   memcpy(heading, p, n);
   heading[n] = '\0';
   *depth_out = depth;
   return 1;
}

static void build_heading_path(char headings[7][CORPUS_SECTION_HEADING], int depth, char *out,
                               size_t cap)
{
   out[0] = '\0';
   for (int i = 1; i <= depth; i++)
   {
      if (!headings[i][0])
         continue;
      if (out[0])
         strncat(out, " > ", cap - strlen(out) - 1);
      strncat(out, headings[i], cap - strlen(out) - 1);
   }
}

int db2_corpus_sections_rebuild(int64_t doc_id)
{
   corpus_doc_text_t doc;
   if (corpus_doc_load(doc_id, &doc) != 0)
      return -1;
   if (delete_sections_for_doc(doc_id) != 0)
      return -1;

   const char *text = doc.normalized_text;
   int64_t text_len = (int64_t)strlen(text);
   int64_t section_ids[7] = {0};
   int64_t section_starts[7] = {0};
   int64_t ordinals[7] = {0};
   char headings[7][CORPUS_SECTION_HEADING];
   memset(headings, 0, sizeof(headings));

   int count = 0;
   const char *p = text;
   while (*p)
   {
      const char *line_start = p;
      const char *line_end = strchr(p, '\n');
      size_t line_len = line_end ? (size_t)(line_end - line_start + 1) : strlen(line_start);
      char line[1024];
      size_t n = line_len < sizeof(line) - 1 ? line_len : sizeof(line) - 1;
      memcpy(line, line_start, n);
      line[n] = '\0';

      int depth = 0;
      char heading[CORPUS_SECTION_HEADING];
      if (parse_heading(line, &depth, heading, sizeof(heading)))
      {
         int64_t offset = (int64_t)(line_start - text);
         for (int d = depth; d <= 6; d++)
         {
            if (section_ids[d] > 0)
               update_section_end(section_ids[d], section_starts[d], offset, text);
            section_ids[d] = 0;
            section_starts[d] = 0;
            ordinals[d] = 0;
            headings[d][0] = '\0';
         }
         db2_copy_text(headings[depth], sizeof(headings[depth]), heading);
         for (int d = depth + 1; d <= 6; d++)
            headings[d][0] = '\0';

         char heading_path[CORPUS_SECTION_PATH];
         build_heading_path(headings, depth, heading_path, sizeof(heading_path));
         int64_t parent_id = 0;
         for (int d = depth - 1; d >= 1; d--)
         {
            if (section_ids[d] > 0)
            {
               parent_id = section_ids[d];
               break;
            }
         }
         int64_t ordinal = ++ordinals[depth];
         char h[65];
         hash_span(text, offset, text_len, h, sizeof(h));
         int64_t new_id = insert_section(doc_id, parent_id, ordinal, depth, heading, heading_path,
                                         offset, text_len, h);
         if (new_id <= 0)
            return -1;
         section_ids[depth] = new_id;
         section_starts[depth] = offset;
         count++;
      }
      p = line_end ? line_end + 1 : line_start + strlen(line_start);
   }

   for (int d = 1; d <= 6; d++)
   {
      if (section_ids[d] > 0)
         update_section_end(section_ids[d], section_starts[d], text_len, text);
   }
   return count;
}

int db2_corpus_sections_list(int64_t doc_id, db2_corpus_section_t *out, int max_out)
{
   void *conn = db2_conn();
   if (!conn || !out || max_out <= 0)
      return -1;
   char err[CS_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT id, doc_id, coalesce(parent_id,0), ordinal, depth, heading, heading_path,"
       "       span_start, span_end, content_hash"
       " FROM document_sections WHERE doc_id = ?1 ORDER BY span_start ASC, id ASC",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", doc_id);
   int n = 0;
   while (n < max_out && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_corpus_section_t *r = &out[n++];
      memset(r, 0, sizeof(*r));
      r->id = aimee_pg_column_int64(st, 0);
      r->doc_id = aimee_pg_column_int64(st, 1);
      r->parent_id = aimee_pg_column_int64(st, 2);
      r->ordinal = aimee_pg_column_int64(st, 3);
      r->depth = aimee_pg_column_int(st, 4);
      db2_copy_text(r->heading, sizeof(r->heading), aimee_pg_column_text(st, 5));
      db2_copy_text(r->heading_path, sizeof(r->heading_path), aimee_pg_column_text(st, 6));
      r->span_start = aimee_pg_column_int64(st, 7);
      r->span_end = aimee_pg_column_int64(st, 8);
      db2_copy_text(r->content_hash, sizeof(r->content_hash), aimee_pg_column_text(st, 9));
   }
   aimee_pg_finalize(st);
   return n;
}

static int64_t find_section_for_offset(int64_t doc_id, int64_t offset)
{
   void *conn = db2_conn();
   char err[CS_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT id FROM document_sections"
                        " WHERE doc_id = ?1 AND span_start <= ?2 AND span_end >= ?2"
                        " ORDER BY depth DESC, span_start DESC LIMIT 1",
                        err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", doc_id);
   aimee_pg_bind_int64(st, "?2", offset);
   int64_t id = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

static int64_t resolve_target_doc(const char *raw_target, int64_t self_doc_id)
{
   void *conn = db2_conn();
   if (!conn || !raw_target || !raw_target[0])
      return 0;

   const char *base = strrchr(raw_target, '/');
   base = base ? base + 1 : raw_target;
   if (strstr(raw_target, "://"))
      return 0;

   char err[CS_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT id FROM docs"
       " WHERE id != ?1 AND (filename = ?2 OR filename LIKE ?3 OR content_hash = ?2)"
       " ORDER BY id ASC LIMIT 1",
       err, sizeof(err));
   if (!st)
      return 0;
   char like[600];
   snprintf(like, sizeof(like), "%%%s", base);
   aimee_pg_bind_int64(st, "?1", self_doc_id);
   aimee_pg_bind_text(st, "?2", raw_target);
   aimee_pg_bind_text(st, "?3", like);
   int64_t id = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

static int64_t insert_reference(int64_t from_doc_id, int64_t from_section_id, const char *ref_type,
                                const char *raw_target, int64_t to_doc_id, const char *resolution,
                                double confidence)
{
   void *conn = db2_conn();
   char err[CS_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "INSERT INTO document_references"
       " (from_doc_id, from_section_id, ref_type, raw_target, to_doc_id, resolution, confidence)"
       " VALUES (?1,?2,?3,?4,?5,?6,?7)"
       " ON CONFLICT (from_doc_id, ref_type, raw_target) DO UPDATE SET"
       "   from_section_id=excluded.from_section_id,"
       "   to_doc_id=excluded.to_doc_id,"
       "   resolution=excluded.resolution,"
       "   confidence=excluded.confidence"
       " RETURNING id",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", from_doc_id);
   if (from_section_id > 0)
      aimee_pg_bind_int64(st, "?2", from_section_id);
   else
      aimee_pg_bind_null(st, "?2");
   aimee_pg_bind_text(st, "?3", ref_type);
   aimee_pg_bind_text(st, "?4", raw_target);
   if (to_doc_id > 0)
      aimee_pg_bind_int64(st, "?5", to_doc_id);
   else
      aimee_pg_bind_null(st, "?5");
   aimee_pg_bind_text(st, "?6", resolution);
   aimee_pg_bind_double(st, "?7", confidence);
   int64_t id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

static int delete_references_from_doc(int64_t doc_id)
{
   void *conn = db2_conn();
   char err[CS_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "DELETE FROM document_references WHERE from_doc_id = ?1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", doc_id);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

static void clean_ref_target(char *s)
{
   size_t n = strlen(s);
   while (n > 0 && (s[n - 1] == ')' || s[n - 1] == ',' || s[n - 1] == '.' || s[n - 1] == ';' ||
                    isspace((unsigned char)s[n - 1])))
      s[--n] = '\0';
}

static int maybe_add_reference(int64_t doc_id, int64_t offset, const char *raw, const char *type,
                               double confidence)
{
   char target[CORPUS_REF_TARGET_LEN];
   db2_copy_text(target, sizeof(target), raw);
   clean_ref_target(target);
   if (!target[0])
      return 0;
   int64_t section_id = find_section_for_offset(doc_id, offset);
   int64_t to_doc_id = resolve_target_doc(target, doc_id);
   const char *resolution =
       strstr(target, "://") ? "external" : (to_doc_id > 0 ? "resolved" : "unresolved");
   return insert_reference(doc_id, section_id, type, target, to_doc_id, resolution, confidence) > 0;
}

int db2_corpus_extract_references(int64_t doc_id)
{
   corpus_doc_text_t doc;
   if (corpus_doc_load(doc_id, &doc) != 0)
      return -1;
   if (delete_references_from_doc(doc_id) != 0)
      return -1;

   int count = 0;
   const char *text = doc.normalized_text;

   for (const char *p = text; (p = strstr(p, "](")) != NULL; p += 2)
   {
      const char *start = p + 2;
      const char *end = strchr(start, ')');
      if (!end || end == start)
         continue;
      char target[CORPUS_REF_TARGET_LEN];
      size_t n = (size_t)(end - start);
      if (n >= sizeof(target))
         n = sizeof(target) - 1;
      memcpy(target, start, n);
      target[n] = '\0';
      if (maybe_add_reference(doc_id, (int64_t)(start - text), target, "cites", 1.0) > 0)
         count++;
   }

   for (const char *p = text; (p = strcasestr(p, "see ")) != NULL; p += 4)
   {
      const char *start = p + 4;
      while (*start == ' ' || *start == '\t')
         start++;
      char target[CORPUS_REF_TARGET_LEN];
      int n = 0;
      while (start[n] && !isspace((unsigned char)start[n]) && n + 1 < (int)sizeof(target))
      {
         target[n] = start[n];
         n++;
      }
      target[n] = '\0';
      if (strchr(target, '.') &&
          maybe_add_reference(doc_id, (int64_t)(start - text), target, "mentions", 0.75) > 0)
         count++;
   }

   return count;
}

int db2_corpus_references_list(int64_t from_doc_id, db2_corpus_reference_t *out, int max_out)
{
   void *conn = db2_conn();
   if (!conn || !out || max_out <= 0)
      return -1;
   char err[CS_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT id, from_doc_id, coalesce(from_section_id,0), ref_type, raw_target,"
                        "       coalesce(to_doc_id,0), resolution, confidence"
                        " FROM document_references WHERE from_doc_id = ?1 ORDER BY id ASC",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", from_doc_id);
   int n = 0;
   while (n < max_out && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_corpus_reference_t *r = &out[n++];
      memset(r, 0, sizeof(*r));
      r->id = aimee_pg_column_int64(st, 0);
      r->from_doc_id = aimee_pg_column_int64(st, 1);
      r->from_section_id = aimee_pg_column_int64(st, 2);
      db2_copy_text(r->ref_type, sizeof(r->ref_type), aimee_pg_column_text(st, 3));
      db2_copy_text(r->raw_target, sizeof(r->raw_target), aimee_pg_column_text(st, 4));
      r->to_doc_id = aimee_pg_column_int64(st, 5);
      db2_copy_text(r->resolution, sizeof(r->resolution), aimee_pg_column_text(st, 6));
      r->confidence = aimee_pg_column_double(st, 7);
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_corpus_mark_references_stale_for_doc(int64_t to_doc_id)
{
   void *conn = db2_conn();
   if (!conn || to_doc_id <= 0)
      return -1;
   char err[CS_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "UPDATE document_references SET resolution = 'stale' WHERE to_doc_id = ?1", err,
       sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", to_doc_id);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   int changed = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   if (rc != AIMEE_PG_DONE && rc != AIMEE_PG_ROW)
      return -1;
   return changed;
}
