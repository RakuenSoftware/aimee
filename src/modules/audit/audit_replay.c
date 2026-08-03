/* audit_replay.c: read an audit-on-bus capture file and re-present the
 * governed-action rows. See audit_replay.h.
 *
 * Two consumers: the aimee-server --audit-replay CLI (text, obs_bus_replay_print)
 * and the /v1/audit endpoints (JSON, audit_replay_to_json / audit_replay_capture_list).
 */
#include <aimee/audit/audit_replay.h>

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <aimee/audit/obs_bus.h> /* OBS_BUS_KIND_ACTION */
#include <aimee/core/event_bus/bus_capture.h>
#include "cJSON.h"

#define AB_CAP_PREFIX "audit-bus-capture-"
#define AB_CAP_SUFFIX ".aimeecap"

/* Byte budget for a JSON replay page. The /v1 reply shares a 256 KB buffer
 * (SHTTP_RESP_MAX); a row-count limit alone cannot bound the payload because rows
 * carry variable-length fields, so audit_replay_to_json also stops materializing
 * once the accumulated size approaches this budget and reports truncated=true so
 * the client pages via offset. Kept well under 256 KB to leave room for the
 * envelope and cJSON's escaping expansion. */
#define AB_REPLAY_JSON_BUDGET (200 * 1024)

/* Read a length-prefixed string from the audit-row payload (obs_bus.c's wire
 * form: 7 length-prefixed strings then an int64 task id). Returns new offset, or
 * 0 on malformed input. */
static uint32_t get_str(const uint8_t *b, uint32_t off, uint32_t len, char *out, uint32_t cap)
{
   if (off + 4 > len)
      return 0;
   uint32_t l;
   memcpy(&l, b + off, 4);
   off += 4;
   if (off + l > len || l >= cap)
      return 0;
   memcpy(out, b + off, l);
   out[l] = '\0';
   return off + l;
}

/* Decode one audit-action payload into its fields. Returns 1 on success. */
static int decode_row(const uint8_t *p, uint32_t len, char *actor, char *tool, char *hash,
                      char *command, char *mode, char *reason, char *verdict, int64_t *task_id)
{
   uint32_t off = 0;
   if (!(off = get_str(p, off, len, actor, 128)) || !(off = get_str(p, off, len, tool, 256)) ||
       !(off = get_str(p, off, len, hash, 96)) || !(off = get_str(p, off, len, command, 512)) ||
       !(off = get_str(p, off, len, mode, 64)) || !(off = get_str(p, off, len, reason, 128)) ||
       !(off = get_str(p, off, len, verdict, 32)) || off + 8 > len)
      return 0;
   memcpy(task_id, p + off, 8);
   return 1;
}

/* Read a whole file into a malloc'd buffer. Returns NULL on any error. */
static uint8_t *read_all(const char *path, size_t *out_size)
{
   int fd = open(path, O_RDONLY);
   if (fd < 0)
      return NULL;
   struct stat st;
   if (fstat(fd, &st) != 0 || st.st_size < 0)
   {
      close(fd);
      return NULL;
   }
   size_t size = (size_t)st.st_size;
   uint8_t *buf = malloc(size ? size : 1);
   if (!buf)
   {
      close(fd);
      return NULL;
   }
   size_t got = 0;
   while (got < size)
   {
      ssize_t r = read(fd, buf + got, size - got);
      if (r <= 0)
         break;
      got += (size_t)r;
   }
   close(fd);
   if (got != size)
   {
      free(buf);
      return NULL;
   }
   *out_size = size;
   return buf;
}

/* ---- text replay (aimee-server --audit-replay) ---- */

struct text_sink
{
   FILE *out;
   uint64_t rows;
   uint64_t malformed;
};

static void on_record_text(void *ctx, const bus_capture_event_t *ev)
{
   struct text_sink *s = ctx;
   if (ev->type != BUS_CAP_EVENT || ev->frame.event_kind != OBS_BUS_KIND_ACTION)
      return;
   char actor[128], tool[256], hash[96], command[512], mode[64], reason[128], verdict[32];
   int64_t task_id;
   if (!decode_row(ev->payload, ev->payload_len, actor, tool, hash, command, mode, reason, verdict,
                   &task_id))
   {
      s->malformed++;
      return;
   }
   /* Defense in depth: the emit-side serializer (obs_bus.c put_str) already scrubs
    * control bytes, but a legacy or hand-crafted capture could still carry a raw
    * newline / ANSI escape in an identity field. Scrub before printing so a dump
    * to a terminal cannot be made to inject escapes or forge a row. */
   for (char *p = actor; *p; p++)
      if ((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7f)
         *p = '?';
   for (char *p = tool; *p; p++)
      if ((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7f)
         *p = '?';
   for (char *p = mode; *p; p++)
      if ((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7f)
         *p = '?';
   for (char *p = reason; *p; p++)
      if ((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7f)
         *p = '?';
   for (char *p = command; *p; p++)
      if ((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7f)
         *p = '?';
   if (s->out)
      fprintf(s->out,
              "seq=%llu verdict=%-8s actor=%-8s tool=%s mode=%s reason=%s task_id=%lld "
              "args_hash=%s command=\"%s\"\n",
              (unsigned long long)ev->frame.seq, verdict, actor, tool, mode, reason,
              (long long)task_id, hash, command);
   s->rows++;
}

int obs_bus_replay_print(const char *path, FILE *out)
{
   size_t size = 0;
   uint8_t *buf = read_all(path, &size);
   if (!buf)
   {
      if (out)
         fprintf(out, "audit-replay: cannot read %s\n", path);
      return -1;
   }
   struct text_sink s = {.out = out, .rows = 0, .malformed = 0};
   bus_capture_report_t rep = bus_capture_read(buf, size, on_record_text, &s);
   free(buf);

   if (out)
   {
      fprintf(out, "-- %s: stream %s, %llu governed-action row(s) replayed", path,
              bus_capture_status_name(rep.status), (unsigned long long)s.rows);
      if (s.malformed)
         fprintf(out, ", %llu malformed skipped", (unsigned long long)s.malformed);
      if (rep.status == BUS_CAPTURE_TRUNCATED || rep.status == BUS_CAPTURE_CORRUPT)
         fprintf(out, " (last good seq %llu at byte %zu)", (unsigned long long)rep.last_good_seq,
                 rep.offending_off);
      fprintf(out, "\n");
   }

   if (rep.status == BUS_CAPTURE_TRUNCATED || rep.status == BUS_CAPTURE_CORRUPT)
      return -2;
   return 0;
}

/* ---- JSON replay (/v1/audit/replay) ---- */

struct json_sink
{
   cJSON *rows;
   uint64_t total; /* all audit rows in the stream (not just the returned window) */
   uint64_t malformed;
   long offset;   /* first row index to include */
   long limit;    /* max rows to include (<=0 = all) */
   size_t bytes;  /* accumulated estimated JSON size of materialized rows */
   int truncated; /* set when the byte budget stopped materialization early */
};

static void on_record_json(void *ctx, const bus_capture_event_t *ev)
{
   struct json_sink *s = ctx;
   if (ev->type != BUS_CAP_EVENT || ev->frame.event_kind != OBS_BUS_KIND_ACTION)
      return;
   char actor[128], tool[256], hash[96], command[512], mode[64], reason[128], verdict[32];
   int64_t task_id;
   if (!decode_row(ev->payload, ev->payload_len, actor, tool, hash, command, mode, reason, verdict,
                   &task_id))
   {
      s->malformed++;
      return;
   }
   long idx = (long)s->total;
   s->total++;
   /* Windowing: keep total counting every row, but only materialize the
    * [offset, offset+limit) slice so the response stays bounded. */
   if (idx < s->offset || (s->limit > 0 && idx >= s->offset + s->limit))
      return;

   /* Byte budget: a row's fields are variable length, so a row-count window can
    * still overflow the /v1 buffer. Once the estimated size would exceed the
    * budget, stop materializing and report truncated (the client pages onward).
    * Estimate = per-row JSON overhead + the field value lengths (an upper-ish
    * bound; cJSON escaping can add a little, which the budget's headroom covers). */
   size_t est = 120 + strlen(actor) + strlen(tool) + strlen(hash) + strlen(command) + strlen(mode) +
                strlen(reason) + strlen(verdict);
   if (cJSON_GetArraySize(s->rows) > 0 && s->bytes + est > AB_REPLAY_JSON_BUDGET)
   {
      s->truncated = 1;
      return;
   }
   s->bytes += est;

   cJSON *o = cJSON_CreateObject();
   if (!o)
      return;
   cJSON_AddNumberToObject(o, "seq", (double)ev->frame.seq);
   cJSON_AddStringToObject(o, "verdict", verdict);
   cJSON_AddStringToObject(o, "actor", actor);
   cJSON_AddStringToObject(o, "tool", tool);
   cJSON_AddStringToObject(o, "mode", mode);
   cJSON_AddStringToObject(o, "reason", reason);
   cJSON_AddNumberToObject(o, "task_id", (double)task_id);
   cJSON_AddStringToObject(o, "args_hash", hash);
   cJSON_AddStringToObject(o, "command", command);
   cJSON_AddItemToArray(s->rows, o);
}

cJSON *audit_replay_to_json(const char *path, long offset, long limit)
{
   size_t size = 0;
   uint8_t *buf = read_all(path, &size);
   if (!buf)
      return NULL;
   if (offset < 0)
      offset = 0;
   struct json_sink s = {.rows = cJSON_CreateArray(),
                         .total = 0,
                         .malformed = 0,
                         .offset = offset,
                         .limit = limit,
                         .bytes = 0,
                         .truncated = 0};
   bus_capture_report_t rep = bus_capture_read(buf, size, on_record_json, &s);
   free(buf);

   int returned = cJSON_GetArraySize(s.rows);
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "status", bus_capture_status_name(rep.status));
   cJSON_AddNumberToObject(o, "total", (double)s.total);  /* all rows in the stream */
   cJSON_AddNumberToObject(o, "count", (double)returned); /* returned window */
   cJSON_AddNumberToObject(o, "offset", (double)offset);
   if (limit > 0)
      cJSON_AddNumberToObject(o, "limit", (double)limit);
   if (s.truncated)
   {
      /* the byte budget cut the page short; tell the client where to resume */
      cJSON_AddBoolToObject(o, "truncated", 1);
      cJSON_AddNumberToObject(o, "next_offset", (double)(offset + returned));
   }
   if (s.malformed)
      cJSON_AddNumberToObject(o, "malformed", (double)s.malformed);
   if (rep.status == BUS_CAPTURE_TRUNCATED || rep.status == BUS_CAPTURE_CORRUPT)
   {
      cJSON_AddNumberToObject(o, "last_good_seq", (double)rep.last_good_seq);
      cJSON_AddNumberToObject(o, "offending_off", (double)rep.offending_off);
   }
   cJSON_AddItemToObject(o, "rows", s.rows);
   return o;
}

/* Validate a capture filename supplied by a client: basename only (no path
 * traversal) and the audit-capture naming convention. */
int audit_replay_valid_basename(const char *name)
{
   if (!name || !name[0] || strchr(name, '/'))
      return 0;
   size_t nlen = strlen(name), plen = strlen(AB_CAP_PREFIX), slen = strlen(AB_CAP_SUFFIX);
   if (nlen <= plen + slen)
      return 0; /* must have content between prefix and suffix */
   return strncmp(name, AB_CAP_PREFIX, plen) == 0 && strcmp(name + nlen - slen, AB_CAP_SUFFIX) == 0;
}

cJSON *audit_replay_capture_list(const char *dir)
{
   cJSON *arr = cJSON_CreateArray();
   if (!dir || !dir[0])
      return arr;
   DIR *d = opendir(dir);
   if (!d)
      return arr;
   struct dirent *e;
   while ((e = readdir(d)) != NULL)
   {
      if (!audit_replay_valid_basename(e->d_name))
         continue;
      char p[4096];
      snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
      struct stat st;
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "name", e->d_name);
      cJSON_AddNumberToObject(o, "bytes", (stat(p, &st) == 0) ? (double)st.st_size : -1.0);
      cJSON_AddItemToArray(arr, o);
   }
   closedir(d);
   return arr;
}
