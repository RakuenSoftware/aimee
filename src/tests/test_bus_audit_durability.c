/* test_bus_audit_durability.c: the durability invariant for audit-on-bus.
 *
 * The audit migration is all-or-nothing: the direct file write is gone, the bus
 * is the sole route from the emit site to the ledger. That makes ONE property
 * load-bearing — every emitted row that the bus accepted is written to the ledger
 * exactly once, and shutdown drains the in-flight rows rather than losing them.
 * This test holds the real module to that: it starts the audit bus, emits N rows
 * with distinct task ids through the real producer/consumer path, stops (which
 * must drain), then reads the real ledger back with audit_ledger_read and
 * requires exactly N rows, each task id present exactly once, zero drops.
 *
 * It also measures the emit-side enqueue overhead (p50/p99) — the committed
 * budget for an asynchronous publish is what it costs the CALLER, since the write
 * itself is off the caller's thread.
 *
 * A test/integration harness; the bus is not linked into a shipping binary by it.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audit_bus.h"
#include "audit_ledger.h"
#include "cJSON.h"
#include "log.h"

#define N 5000

static uint64_t now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
   uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
   return (x > y) - (x < y);
}
static uint64_t pctl(uint64_t *v, int n, int p)
{
   qsort(v, (size_t)n, sizeof *v, cmp_u64);
   return v[(int)(((int64_t)p * (n - 1)) / 100)];
}

int main(void)
{
   printf("test_bus_audit_durability:\n");

   char home[] = "/tmp/aimee-busaudit-XXXXXX";
   if (!mkdtemp(home))
   {
      fprintf(stderr, "FAIL: tmp home\n");
      return 1;
   }
   setenv("AIMEE_HOME", home, 1);
   audit_log_open(); /* opens audit.log under AIMEE_HOME so the consumer can append */

   if (audit_bus_start() != 0)
   {
      fprintf(stderr, "FAIL: audit_bus_start\n");
      return 1;
   }

   /* Emit N rows over the bus, each with a distinct task id (0..N-1) so the
    * read-back can prove exactly-once with no reliance on ordering. */
   uint64_t *emit_ns = malloc(N * sizeof *emit_ns);
   assert(emit_ns);
   for (int i = 0; i < N; i++)
   {
      char tool[32], hash[32];
      snprintf(tool, sizeof tool, "Tool_%d", i % 7);
      snprintf(hash, sizeof hash, "v1-%d", i);
      uint64_t s = now_ns();
      audit_bus_emit("primary", tool, hash, "cd ; rm", "approve", "read_before_write", "block", i);
      emit_ns[i] = now_ns() - s;
   }

   /* Stop drains every in-flight row before returning. */
   audit_bus_stop();

   uint64_t dropped = audit_bus_dropped();
   uint64_t written = audit_bus_written();
   printf("  emitted %d, written %llu, dropped %llu\n", N, (unsigned long long)written,
          (unsigned long long)dropped);
   if (dropped != 0)
   {
      fprintf(stderr, "FAIL: %llu rows dropped — durability violated\n",
              (unsigned long long)dropped);
      return 1;
   }
   if (written != (uint64_t)N)
   {
      fprintf(stderr, "FAIL: consumer wrote %llu, expected %d\n", (unsigned long long)written, N);
      return 1;
   }

   /* Read the REAL ledger back and require exactly N rows, each task id once. */
   cJSON *rows = audit_ledger_read(NULL, NULL);
   if (!rows)
   {
      fprintf(stderr, "FAIL: audit_ledger_read returned NULL\n");
      return 1;
   }
   int got = cJSON_GetArraySize(rows);
   if (got != N)
   {
      fprintf(stderr, "FAIL: ledger has %d rows, expected %d\n", got, N);
      return 1;
   }
   char *seen = calloc(N, 1);
   assert(seen);
   cJSON *row = NULL;
   int dupes = 0, oob = 0;
   cJSON_ArrayForEach(row, rows)
   {
      cJSON *tid = cJSON_GetObjectItemCaseSensitive(row, "task_id");
      if (!cJSON_IsNumber(tid))
      {
         fprintf(stderr, "FAIL: row without numeric task_id\n");
         return 1;
      }
      int id = (int)tid->valuedouble;
      if (id < 0 || id >= N)
      {
         oob++;
         continue;
      }
      if (seen[id])
         dupes++;
      seen[id] = 1;
   }
   int missing = 0;
   for (int i = 0; i < N; i++)
      if (!seen[i])
         missing++;
   if (dupes || oob || missing)
   {
      fprintf(stderr, "FAIL: exactly-once violated (dupes=%d out-of-range=%d missing=%d)\n", dupes,
              oob, missing);
      return 1;
   }
   printf("  ledger read-back: %d rows, each task id 0..%d present exactly once\n", got, N - 1);

   uint64_t p50 = pctl(emit_ns, N, 50), p99 = pctl(emit_ns, N, 99);
   printf("  emit (enqueue) overhead: p50=%llu ns  p99=%llu ns  (publish cost on the caller;\n"
          "    the ledger write is off the caller's thread)\n",
          (unsigned long long)p50, (unsigned long long)p99);

   cJSON_Delete(rows);
   free(seen);
   free(emit_ns);
   printf("test_bus_audit_durability: OK (audit rows cross the bus exactly once, lossless "
          "shutdown)\n");
   return 0;
}
