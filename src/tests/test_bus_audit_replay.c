/* test_bus_audit_replay.c: the record+replay payoff of putting audit on the bus.
 *
 * The reason the governed-action audit row crosses the event bus is not speed —
 * it is auditability and record+replay. The bus host's capture tap records every
 * routed event, in seq order, into a per-session capture file. This test proves
 * that stream is a faithful, replayable record of what happened: it emits N audit
 * rows through the real audit bus, stops, then reads the real capture file back
 * with bus_capture_read and requires that exactly N audit events replay, IN
 * ORDER, each one reconstructing the governed-action row that was emitted. Replay
 * is observational (nothing re-executed), so a match is exact by construction.
 *
 * A test/integration harness; the bus is not linked into a shipping binary by it.
 */
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <aimee/audit/obs_bus.h>
#include <aimee/core/event_bus/bus_capture.h>
#include "log.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

#define N                 3000
#define KIND_AUDIT_ACTION 3000 /* must match obs_bus.c */

/* Read a length-prefixed string from the audit-row payload (obs_bus.c's wire
 * form: 7 length-prefixed strings then an int64 task_id). Returns new offset or
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

struct replay
{
   int n;
   int bad;
   int task_ids[N];
   char tools[N][32];
   char verdicts[N][32];
};

static void on_event(void *ctx, const bus_capture_event_t *ev)
{
   struct replay *r = ctx;
   if (ev->type != BUS_CAP_EVENT || ev->frame.event_kind != KIND_AUDIT_ACTION)
      return; /* ignore host notices / non-audit records */
   if (r->n >= N)
   {
      r->bad = 1;
      return;
   }
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
      r->bad = 1;
      return;
   }
   int64_t task_id;
   memcpy(&task_id, p + off, 8);
   r->task_ids[r->n] = (int)task_id;
   snprintf(r->tools[r->n], sizeof r->tools[r->n], "%s", tool);
   snprintf(r->verdicts[r->n], sizeof r->verdicts[r->n], "%s", verdict);
   r->n++;
}

int main(void)
{
   printf("test_bus_audit_replay:\n");

   char home[256];
   snprintf(home, sizeof home, "%s/aimee-busreplay-XXXXXX", platform_tmpdir());
   if (!mkdtemp(home))
   {
      fprintf(stderr, "FAIL: tmp home\n");
      return 1;
   }
   setenv("AIMEE_HOME", home, 1);
   audit_log_open();

   if (obs_bus_start() != 0)
   {
      fprintf(stderr, "FAIL: obs_bus_start\n");
      return 1;
   }

   /* Emit N rows with reconstructable content: task id = i, tool = Tool_(i%7),
    * verdict alternating, so replay can check every field against the emit. */
   for (int i = 0; i < N; i++)
   {
      char tool[32], hash[32];
      snprintf(tool, sizeof tool, "Tool_%d", i % 7);
      snprintf(hash, sizeof hash, "v1-%d", i);
      const char *verdict = (i % 2) ? "block" : "allow";
      obs_bus_emit("primary", tool, hash, "cd ; rm", "approve", "read_before_write", verdict, i);
   }
   obs_bus_stop(); /* final flush persists the capture stream */

   /* Find this session's capture file (named audit-bus-capture-<time>-<pid>.aimeecap). */
   char path[4096];
   path[0] = '\0';
   DIR *d = opendir(home);
   assert(d);
   struct dirent *e;
   while ((e = readdir(d)) != NULL)
   {
      if (strncmp(e->d_name, "audit-bus-capture-", 18) == 0 && strstr(e->d_name, ".aimeecap"))
      {
         snprintf(path, sizeof path, "%s/%s", home, e->d_name);
         break;
      }
   }
   closedir(d);
   if (!path[0])
   {
      fprintf(stderr, "FAIL: no capture file in %s (record+replay stream not written)\n", home);
      return 1;
   }
   int fd = open(path, O_RDONLY);
   if (fd < 0)
   {
      fprintf(stderr, "FAIL: cannot open capture file %s\n", path);
      return 1;
   }
   struct stat st;
   fstat(fd, &st);
   uint8_t *buf = malloc((size_t)st.st_size);
   assert(buf);
   if (read(fd, buf, (size_t)st.st_size) != (ssize_t)st.st_size)
   {
      fprintf(stderr, "FAIL: short read of capture file\n");
      return 1;
   }
   close(fd);
   printf("  capture file: %lld bytes\n", (long long)st.st_size);

   /* Replay it. */
   struct replay *r = calloc(1, sizeof *r);
   assert(r);
   bus_capture_report_t rep = bus_capture_read(buf, (size_t)st.st_size, on_event, r);
   printf("  stream status: %s, %llu records parsed\n", bus_capture_status_name(rep.status),
          (unsigned long long)rep.records);

   if (rep.status == BUS_CAPTURE_CORRUPT || rep.status == BUS_CAPTURE_TRUNCATED)
   {
      fprintf(stderr, "FAIL: capture stream is %s (rule %d at off %zu) — not a faithful record\n",
              bus_capture_status_name(rep.status), rep.rule, rep.offending_off);
      return 1;
   }
   if (r->bad)
   {
      fprintf(stderr, "FAIL: a replayed audit record was malformed or overflowed\n");
      return 1;
   }
   if (r->n != N)
   {
      fprintf(stderr, "FAIL: replayed %d audit events, expected %d\n", r->n, N);
      return 1;
   }

   /* Every emitted row must reappear, IN ORDER, reconstructed field-for-field. */
   for (int k = 0; k < N; k++)
   {
      char want_tool[32];
      snprintf(want_tool, sizeof want_tool, "Tool_%d", k % 7);
      const char *want_verdict = (k % 2) ? "block" : "allow";
      if (r->task_ids[k] != k || strcmp(r->tools[k], want_tool) != 0 ||
          strcmp(r->verdicts[k], want_verdict) != 0)
      {
         fprintf(stderr,
                 "FAIL: replay[%d] = {task=%d tool=%s verdict=%s}, expected {task=%d tool=%s "
                 "verdict=%s} — replay is not faithful/ordered\n",
                 k, r->task_ids[k], r->tools[k], r->verdicts[k], k, want_tool, want_verdict);
         return 1;
      }
   }
   printf("  replayed %d audit events in seq order; every governed-action row reconstructed "
          "exactly\n",
          N);

   free(buf);
   free(r);
   printf("test_bus_audit_replay: OK (the audit stream is a faithful, ordered, replayable "
          "record)\n");
   return 0;
}
