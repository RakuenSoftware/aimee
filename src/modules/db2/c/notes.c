/* db2/notes.c: investigation notes — Postgres via libpq. */

#include "notes.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Slug generation --- */

void db2_note_title_to_slug(const char *title, char *slug, size_t slug_len)
{
   if (!title || !slug || slug_len == 0)
      return;

   size_t max = slug_len - 1;
   size_t j = 0;
   int prev_dash = 1;

   for (size_t i = 0; title[i] && j < max; i++)
   {
      char c = title[i];
      if (isalnum((unsigned char)c))
      {
         slug[j++] = (char)tolower((unsigned char)c);
         prev_dash = 0;
      }
      else if ((c == ' ' || c == '-' || c == '_') && !prev_dash && j > 0)
      {
         slug[j++] = '-';
         prev_dash = 1;
      }
   }

   if (j > 0 && slug[j - 1] == '-')
      j--;

   slug[j] = '\0';
}

/* --- Row mapping --- */

static void row_to_note(aimee_pg_stmt_t *st, note_t *n)
{
   memset(n, 0, sizeof(*n));
   n->id = aimee_pg_column_int64(st, 0);
   db2_copy_col_text(n->title, sizeof(n->title), st, 1);
   db2_copy_col_text(n->slug, sizeof(n->slug), st, 2);
   db2_copy_col_text(n->content, sizeof(n->content), st, 3);
   db2_copy_col_text(n->tags, sizeof(n->tags), st, 4);
   db2_copy_col_text(n->author, sizeof(n->author), st, 5);
   db2_copy_col_text(n->created_at, sizeof(n->created_at), st, 6);
   db2_copy_col_text(n->updated_at, sizeof(n->updated_at), st, 7);
}

/* --- CRUD --- */

int db2_note_create(const char *title, const char *content, const char *tags, const char *author,
                    note_t *out)
{
   void *conn = db2_conn();
   if (!conn || !title || !content)
      return -1;

   char slug[NOTE_MAX_SLUG];
   db2_note_title_to_slug(title, slug, sizeof(slug));
   if (slug[0] == '\0')
      return -1;

   char err[256] = "";
   /* Check if a note with this slug already exists; if so, append. */
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT id, title, slug, content, tags, author, created_at, updated_at "
                        "FROM notes WHERE slug = ?1",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", slug);

   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      note_t existing;
      row_to_note(st, &existing);
      aimee_pg_finalize(st);

      char merged[NOTE_MAX_CONTENT];
      snprintf(merged, sizeof(merged), "%s\n\n%s", existing.content, content);
      const char *final_tags = (tags && tags[0]) ? tags : existing.tags;

      aimee_pg_stmt_t *us = aimee_pg_prepare(conn,
                                             "UPDATE notes SET content = ?1, tags = ?2, "
                                             "updated_at = pg_now_text() WHERE id = ?3",
                                             err, sizeof(err));
      if (!us)
         return -1;
      aimee_pg_bind_text(us, "?1", merged);
      aimee_pg_bind_text(us, "?2", final_tags);
      aimee_pg_bind_int64(us, "?3", existing.id);
      aimee_pg_step_t rc = aimee_pg_step(us, err, sizeof(err));
      aimee_pg_finalize(us);
      if (rc != AIMEE_PG_DONE)
         return -1;

      if (out)
         return db2_note_get(existing.id, out);
      return 0;
   }
   aimee_pg_finalize(st);

   /* New note. RETURNING id keeps the recovery path explicit. */
   aimee_pg_stmt_t *is = aimee_pg_prepare(conn,
                                          "INSERT INTO notes (title, slug, content, tags, author) "
                                          "VALUES (?1, ?2, ?3, ?4, ?5) RETURNING id",
                                          err, sizeof(err));
   if (!is)
      return -1;
   aimee_pg_bind_text(is, "?1", title);
   aimee_pg_bind_text(is, "?2", slug);
   aimee_pg_bind_text(is, "?3", content);
   aimee_pg_bind_text(is, "?4", tags ? tags : "");
   aimee_pg_bind_text(is, "?5", author ? author : "");

   int64_t new_id = -1;
   if (aimee_pg_step(is, err, sizeof(err)) == AIMEE_PG_ROW)
      new_id = aimee_pg_column_int64(is, 0);
   aimee_pg_finalize(is);
   if (new_id < 0)
      return -1;

   if (out)
      return db2_note_get(new_id, out);
   return 0;
}

int db2_note_list(const char *tag, int limit, note_t *out, int max)
{
   void *conn = db2_conn();
   if (!conn || !out || max <= 0)
      return 0;
   if (limit <= 0 || limit > max)
      limit = max;

   char err[256] = "";
   aimee_pg_stmt_t *st = NULL;
   if (tag && tag[0])
   {
      st = aimee_pg_prepare(conn,
                            "SELECT id, title, slug, content, tags, author, created_at, "
                            "updated_at FROM notes "
                            "WHERE ',' || tags || ',' LIKE '%,' || ?1 || ',%' "
                            "ORDER BY updated_at DESC LIMIT ?2",
                            err, sizeof(err));
      if (!st)
         return 0;
      aimee_pg_bind_text(st, "?1", tag);
      aimee_pg_bind_int(st, "?2", limit);
   }
   else
   {
      st = aimee_pg_prepare(conn,
                            "SELECT id, title, slug, content, tags, author, created_at, "
                            "updated_at FROM notes ORDER BY updated_at DESC LIMIT ?1",
                            err, sizeof(err));
      if (!st)
         return 0;
      aimee_pg_bind_int(st, "?1", limit);
   }

   int count = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && count < limit)
      row_to_note(st, &out[count++]);

   aimee_pg_finalize(st);
   return count;
}

int db2_note_search(const char *query, note_t *out, int max)
{
   void *conn = db2_conn();
   if (!conn || !query || !out || max <= 0)
      return 0;

   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT id, title, slug, content, tags, author, created_at, "
                        "updated_at FROM notes "
                        "WHERE content LIKE '%' || ?1 || '%' OR title LIKE '%' || ?2 || '%' "
                        "ORDER BY updated_at DESC LIMIT ?3",
                        err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", query);
   aimee_pg_bind_text(st, "?2", query);
   aimee_pg_bind_int(st, "?3", max);

   int count = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && count < max)
      row_to_note(st, &out[count++]);

   aimee_pg_finalize(st);
   return count;
}

int db2_note_get(int64_t id, note_t *out)
{
   void *conn = db2_conn();
   if (!conn || !out)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT id, title, slug, content, tags, author, "
                                          "created_at, updated_at FROM notes WHERE id = ?1",
                                          err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", id);
   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW) ? 0 : -1;
   if (rc == 0)
      row_to_note(st, out);
   aimee_pg_finalize(st);
   return rc;
}

int db2_note_delete(int64_t id)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "DELETE FROM notes WHERE id = ?1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", id);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}
