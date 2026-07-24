/* audit_replay.c: read an audit-on-bus capture file and re-present the
 * governed-action rows. See audit_replay.h. */
#include "audit_replay.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fcntl.h>

#include "audit_bus.h" /* AUDIT_BUS_KIND_ACTION */
#include "bus_capture.h"

/* Read a length-prefixed string from the audit-row payload (audit_bus.c's wire
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

struct sink
{
   FILE *out;
   uint64_t rows;
   uint64_t malformed;
};

static void on_record(void *ctx, const bus_capture_event_t *ev)
{
   struct sink *s = ctx;
   if (ev->type != BUS_CAP_EVENT || ev->frame.event_kind != AUDIT_BUS_KIND_ACTION)
      return; /* host notices / non-audit records are not governed-action rows */

   char actor[128], tool[256], hash[96], command[512], mode[64], reason[128], verdict[32];
   uint32_t off = 0;
   const uint8_t *p = ev->payload;
   uint32_t len = ev->payload_len;
   if (!(off = get_str(p, off, len, actor, sizeof actor)) ||
       !(off = get_str(p, off, len, tool, sizeof tool)) ||
       !(off = get_str(p, off, len, hash, sizeof hash)) ||
       !(off = get_str(p, off, len, command, sizeof command)) ||
       !(off = get_str(p, off, len, mode, sizeof mode)) ||
       !(off = get_str(p, off, len, reason, sizeof reason)) ||
       !(off = get_str(p, off, len, verdict, sizeof verdict)) || off + 8 > len)
   {
      s->malformed++;
      return;
   }
   int64_t task_id;
   memcpy(&task_id, p + off, 8);

   if (s->out)
      fprintf(s->out,
              "seq=%llu verdict=%-8s actor=%-8s tool=%s mode=%s reason=%s task_id=%lld "
              "args_hash=%s command=\"%s\"\n",
              (unsigned long long)ev->frame.seq, verdict, actor, tool, mode, reason,
              (long long)task_id, hash, command);
   s->rows++;
}

int audit_bus_replay_print(const char *path, FILE *out)
{
   int fd = open(path, O_RDONLY);
   if (fd < 0)
   {
      if (out)
         fprintf(out, "audit-replay: cannot open %s\n", path);
      return -1;
   }
   struct stat st;
   if (fstat(fd, &st) != 0 || st.st_size < 0)
   {
      close(fd);
      return -1;
   }
   size_t size = (size_t)st.st_size;
   uint8_t *buf = malloc(size ? size : 1);
   if (!buf)
   {
      close(fd);
      return -1;
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
      if (out)
         fprintf(out, "audit-replay: short read of %s\n", path);
      return -1;
   }

   struct sink s = {.out = out, .rows = 0, .malformed = 0};
   bus_capture_report_t rep = bus_capture_read(buf, size, on_record, &s);
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
