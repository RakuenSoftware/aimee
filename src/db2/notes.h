/* db2/notes.h: investigation notes, DB2 subsystem.
 *
 * Pure domain API. No backend types. SQL is encapsulated in
 * src/db2/notes.c, which goes through the libpq shim via
 * aimee_pg_* helpers. */
#ifndef DEC_DB2_NOTES_H
#define DEC_DB2_NOTES_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define NOTE_MAX_TITLE   256
#define NOTE_MAX_SLUG    64
#define NOTE_MAX_CONTENT 8192
#define NOTE_MAX_TAGS    256
#define NOTE_MAX_AUTHOR  128

   typedef struct
   {
      int64_t id;
      char title[NOTE_MAX_TITLE];
      char slug[NOTE_MAX_SLUG];
      char content[NOTE_MAX_CONTENT];
      char tags[NOTE_MAX_TAGS];
      char author[NOTE_MAX_AUTHOR];
      char created_at[32];
      char updated_at[32];
   } note_t;

   /* Slug generation: title -> kebab-case, truncated to slug_len-1 chars. */
   void db2_note_title_to_slug(const char *title, char *slug, size_t slug_len);

   /* Create or append to a note. If a note with the same slug exists, appends
    * content to it. Returns 0 on success, -1 on error. */
   int db2_note_create(const char *title, const char *content, const char *tags, const char *author,
                       note_t *out);

   /* List notes, optionally filtered by tag. Returns count written to out. */
   int db2_note_list(const char *tag, int limit, note_t *out, int max);

   /* Search notes by content/title substring. Returns count of matching notes. */
   int db2_note_search(const char *query, note_t *out, int max);

   /* Get a single note by ID. Returns 0 on success, -1 if not found. */
   int db2_note_get(int64_t id, note_t *out);

   /* Delete a note by ID. Returns 0 on success, -1 on error. */
   int db2_note_delete(int64_t id);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_NOTES_H */
