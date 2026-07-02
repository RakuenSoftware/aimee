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

typedef struct
{
   cJSON *obj;    /* the row object (borrowed until moved into the result) */
   char ts[40];   /* sort key 1: ISO-8601 (lexicographic) */
   int file_rank; /* sort key 2: older file first (chronological) */
   long offset;   /* sort key 3: line order within a file */
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
 * chronologically (older first). Returns the number of unparseable lines seen. */
static int read_file(const char *path, int file_rank, ledger_row_t *rows, int *nrows,
                     const char *from_ts, const char *to_ts)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return 0;
   int bad = 0;
   long offset = 0;
   char line[LEDGER_LINE_MAX];
   while (*nrows < LEDGER_MAX_ROWS && fgets(line, sizeof line, f))
   {
      long this_off = offset;
      offset++;
      if (!line[0] || line[0] == '\n')
         continue;
      cJSON *obj = cJSON_Parse(line);
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
      r->offset = this_off;
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
   if (ra->offset != rb->offset)
      return ra->offset < rb->offset ? -1 : 1;
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

   const char *dir = config_default_dir();
   char path[4096];

   /* Chronological order (oldest first): audit.log.N .. audit.log.0, then the
    * current audit.log. file_rank increases with recency so equal-ts rows keep
    * their chronological order. */
   int rank = 0;
   for (int i = LEDGER_MAX_ROTATED; i >= 0; i--)
   {
      snprintf(path, sizeof path, "%s/audit.log.%d", dir, i);
      bad += read_file(path, rank++, rows, &nrows, from_ts, to_ts);
   }
   snprintf(path, sizeof path, "%s/audit.log", dir);
   bad += read_file(path, rank, rows, &nrows, from_ts, to_ts);

   qsort(rows, (size_t)nrows, sizeof *rows, cmp_row);
   for (int i = 0; i < nrows; i++)
      cJSON_AddItemToArray(out, rows[i].obj); /* transfers ownership */
   free(rows);

   if (bad > 0)
      audit_log("audit_ledger", "skipped %d unparseable audit.log line(s)", bad);
   return out;
}
