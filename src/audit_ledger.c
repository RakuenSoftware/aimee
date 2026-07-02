/* audit_ledger.c: read the governed-action audit ledger. See audit_ledger.h. */
#include "audit_ledger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "log.h"

/* Rotation depth mirrors log.c's AUDIT_MAX_FILES (audit.log.0 .. .N). Kept as a
 * local upper bound so the reader tolerates however many rotated files exist. */
#define LEDGER_MAX_ROTATED 8
#define LEDGER_MAX_ROWS    20000
#define LEDGER_LINE_MAX    8192
/* "YYYY-MM-DDThh:mm:ssZ" is 20 bytes; fractional/extended forms fit in 40. */
#define LEDGER_TS_MAX 40

typedef struct
{
   cJSON *obj;             /* the row object (borrowed until moved into the result) */
   char ts[LEDGER_TS_MAX]; /* sort key 1: ISO-8601 (lexicographic) */
   int file_rank;          /* sort key 2: older file first (chronological) */
   long line_seq;          /* sort key 3: logical-line order within a file */
} ledger_row_t;

static int ts_in_window(const char *ts, const char *from_ts, const char *to_ts)
{
   if (from_ts && from_ts[0] && strcmp(ts, from_ts) < 0)
      return 0;
   if (to_ts && to_ts[0] && strcmp(ts, to_ts) > 0)
      return 0;
   return 1;
}

/* Read one file's tool_action rows into rows[] (bounded). file_rank orders files
 * chronologically (older first). Returns the number of unparseable lines seen;
 * sets *truncated if the LEDGER_MAX_ROWS cap was hit. */
static int read_file(const char *path, int file_rank, ledger_row_t *rows, int *nrows,
                     const char *from_ts, const char *to_ts, int *truncated)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return 0;
   int bad = 0;
   long seq = 0;
   char line[LEDGER_LINE_MAX];
   while (fgets(line, sizeof line, f))
   {
      size_t len = strlen(line);
      /* A record with no trailing newline that isn't EOF was longer than the
       * buffer: drain the rest of the physical line and count it ONCE as
       * unparseable — never parse a fragment. */
      if ((len == 0 || line[len - 1] != '\n') && !feof(f))
      {
         int c;
         while ((c = fgetc(f)) != '\n' && c != EOF)
         {
         }
         bad++;
         seq++;
         continue;
      }
      long this_seq = seq++;
      /* Strip the trailing line-ending/whitespace so the strict parse sees only
       * the JSON value (require_null_terminated then rejects trailing garbage
       * without tripping on the newline). */
      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ' ||
                         line[len - 1] == '\t'))
         line[--len] = '\0';
      if (len == 0)
         continue;
      /* Cap check BEFORE parsing, so a parsed object can never be orphaned. */
      if (*nrows >= LEDGER_MAX_ROWS)
      {
         *truncated = 1;
         break;
      }
      /* Strict parse: require the whole line be consumed (require_null_terminated),
       * so trailing garbage or a JSON prefix in a fragment is rejected, not accepted. */
      cJSON *obj = cJSON_ParseWithOpts(line, NULL, 1);
      if (!obj)
      {
         bad++;
         continue;
      }
      cJSON *kind = cJSON_GetObjectItemCaseSensitive(obj, "kind");
      cJSON *ts = cJSON_GetObjectItemCaseSensitive(obj, "ts");
      if (!cJSON_IsString(kind) || strcmp(kind->valuestring, "tool_action") != 0 ||
          !cJSON_IsString(ts))
      {
         cJSON_Delete(obj); /* legacy {event,detail} rows and others: skip quietly */
         continue;
      }
      if (!ts_in_window(ts->valuestring, from_ts, to_ts))
      {
         cJSON_Delete(obj);
         continue;
      }
      ledger_row_t *r = &rows[(*nrows)++];
      r->obj = obj;
      snprintf(r->ts, sizeof r->ts, "%s", ts->valuestring);
      r->file_rank = file_rank;
      r->line_seq = this_seq;
   }
   fclose(f);
   return bad;
}

static int cmp_row(const void *a, const void *b)
{
   const ledger_row_t *ra = a, *rb = b;
   int c = strcmp(ra->ts, rb->ts);
   if (c)
      return c;
   if (ra->file_rank != rb->file_rank)
      return ra->file_rank < rb->file_rank ? -1 : 1;
   if (ra->line_seq != rb->line_seq)
      return ra->line_seq < rb->line_seq ? -1 : 1;
   return 0;
}

cJSON *audit_ledger_read(const char *from_ts, const char *to_ts)
{
   cJSON *out = cJSON_CreateArray();
   if (!out)
      return NULL;

   ledger_row_t *rows = calloc(LEDGER_MAX_ROWS, sizeof *rows);
   if (!rows)
   {
      cJSON_Delete(out);
      return NULL;
   }
   int nrows = 0;
   int bad = 0;
   int truncated = 0;

   const char *dir = config_default_dir();
   char path[4096];

   /* Chronological order (oldest first): audit.log.N .. audit.log.0, then the
    * current audit.log. file_rank increases with recency so equal-ts rows keep
    * their chronological order. */
   int rank = 0;
   for (int i = LEDGER_MAX_ROTATED; i >= 0; i--)
   {
      snprintf(path, sizeof path, "%s/audit.log.%d", dir, i);
      bad += read_file(path, rank++, rows, &nrows, from_ts, to_ts, &truncated);
   }
   snprintf(path, sizeof path, "%s/audit.log", dir);
   bad += read_file(path, rank, rows, &nrows, from_ts, to_ts, &truncated);

   qsort(rows, (size_t)nrows, sizeof *rows, cmp_row);
   for (int i = 0; i < nrows; i++)
      cJSON_AddItemToArray(out, rows[i].obj); /* transfers ownership (steals the pointer) */
   free(rows);

   if (bad > 0)
      audit_log("audit_ledger", "skipped %d unparseable audit.log line(s)", bad);
   if (truncated)
      audit_log("audit_ledger", "row cap %d hit; older/overflow rows omitted", LEDGER_MAX_ROWS);
   return out;
}
