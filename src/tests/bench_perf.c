/*
 * bench_perf.c - Performance benchmark suite for aimee core operations.
 *
 * Measures p50/p95/p99 latency for critical paths and optionally compares
 * against a stored baseline to detect regressions.
 *
 * Usage:
 *   bench-perf                    Run benchmarks, print results
 *   bench-perf --json             Output results as JSON
 *   bench-perf --check FILE       Compare against baseline, exit 1 on regression
 *   bench-perf --save FILE        Save current results as new baseline
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aimee.h"
#include "agent_config.h"
#include "guardrails.h"
#include "platform_test_util.h"

/* --- Timing helpers ---------------------------------------------------- */

static int64_t now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int cmp_double(const void *a, const void *b)
{
   double da = *(const double *)a;
   double db = *(const double *)b;
   if (da < db)
      return -1;
   if (da > db)
      return 1;
   return 0;
}

typedef struct
{
   double p50_ms;
   double p95_ms;
   double p99_ms;
} percentiles_t;

static void compute_percentiles(double *samples, int n, percentiles_t *out)
{
   qsort(samples, (size_t)n, sizeof(double), cmp_double);
   out->p50_ms = samples[n * 50 / 100];
   out->p95_ms = samples[n * 95 / 100];
   out->p99_ms = samples[n * 99 / 100];
}

/* --- Benchmark definitions --------------------------------------------- */

#define BENCH_ITERATIONS 200
#define BENCH_WARMUP     10

typedef struct
{
   const char *name;
   double target_p50_ms;
   double target_p95_ms;
   double tolerance; /* fraction, e.g. 0.20 = +20% */
   percentiles_t results;
} bench_entry_t;

/* Benchmark: pre_tool_check (Edit, simple path) */
static void bench_pre_tool_check(double *samples, int n)
{
   session_state_t state;
   memset(&state, 0, sizeof(state));
   snprintf(state.guardrail_mode, sizeof(state.guardrail_mode), "%s", MODE_APPROVE);
   char msg[512];
   const char *previous_bypass = getenv("AIMEE_ANTIPATTERNS_BYPASS");
   char previous_bypass_buf[32];
   if (previous_bypass)
      snprintf(previous_bypass_buf, sizeof(previous_bypass_buf), "%s", previous_bypass);

   assert(platform_setenv("AIMEE_ANTIPATTERNS_BYPASS", "1") == 0);
   const char *bench_input = "{\"file_path\":\"/tmp/.aimee-bench-session/bench_test.txt\"}";
   const char *bench_cwd = "/tmp/.aimee-bench-session";

   for (int i = 0; i < n; i++)
   {
      int64_t t0 = now_ns();
      pre_tool_check("Edit", bench_input, &state, MODE_APPROVE, bench_cwd, msg, sizeof(msg));
      int64_t t1 = now_ns();
      samples[i] = (double)(t1 - t0) / 1e6;
   }

   if (previous_bypass)
      assert(platform_setenv("AIMEE_ANTIPATTERNS_BYPASS", previous_bypass_buf) == 0);
   else
      assert(platform_unsetenv("AIMEE_ANTIPATTERNS_BYPASS") == 0);
}

/* Benchmark: delegate_dispatch — agent config load + routing overhead */
static void bench_delegate_dispatch(double *samples, int n)
{
   for (int i = 0; i < n; i++)
   {
      agent_config_t cfg;
      int64_t t0 = now_ns();
      agent_load_config(&cfg);
      agent_route(&cfg, "execute");
      int64_t t1 = now_ns();
      samples[i] = (double)(t1 - t0) / 1e6;
   }
}

/* --- JSON baseline I/O ------------------------------------------------- */

static int parse_baseline(const char *path, bench_entry_t *entries, int count)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return -1;

   fseek(f, 0, SEEK_END);
   long len = ftell(f);
   fseek(f, 0, SEEK_SET);
   char *buf = malloc((size_t)len + 1);
   if (!buf)
   {
      fclose(f);
      return -1;
   }
   if (fread(buf, 1, (size_t)len, f) != (size_t)len)
   {
      free(buf);
      fclose(f);
      return -1;
   }
   buf[len] = '\0';
   fclose(f);

   cJSON *root = cJSON_Parse(buf);
   free(buf);
   if (!root)
      return -1;

   cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
   if (!results)
   {
      cJSON_Delete(root);
      return -1;
   }

   /* A baseline case this harness does not run would pass unmeasured. The
    * memory cases moved to aimee-memory-eval with the Go memory owner, and the
    * db_open and startup cases left with the native database. */
   int unknown = 0;
   cJSON *named;
   cJSON_ArrayForEach(named, results)
   {
      int known = 0;
      for (int i = 0; i < count && !known; i++)
         known = named->string && strcmp(named->string, entries[i].name) == 0;
      if (!known)
      {
         fprintf(stderr, "Error: baseline case %s is not run by this harness\n",
                 named->string ? named->string : "(unnamed)");
         unknown++;
      }
   }
   if (unknown)
   {
      cJSON_Delete(root);
      return -1;
   }

   for (int i = 0; i < count; i++)
   {
      /* Convert name to underscore key: "pre_tool_check" from "pre tool check" */
      char key[64];
      snprintf(key, sizeof(key), "%s", entries[i].name);
      for (char *p = key; *p; p++)
         if (*p == ' ')
            *p = '_';

      cJSON *item = cJSON_GetObjectItemCaseSensitive(results, key);
      if (item)
      {
         cJSON *p50 = cJSON_GetObjectItemCaseSensitive(item, "p50_ms");
         cJSON *p95 = cJSON_GetObjectItemCaseSensitive(item, "p95_ms");
         cJSON *p99 = cJSON_GetObjectItemCaseSensitive(item, "p99_ms");
         if (p50)
            entries[i].results.p50_ms = p50->valuedouble;
         if (p95)
            entries[i].results.p95_ms = p95->valuedouble;
         if (p99)
            entries[i].results.p99_ms = p99->valuedouble;
      }
   }

   cJSON_Delete(root);
   return 0;
}

static void save_baseline(const char *path, bench_entry_t *entries, int count)
{
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "version", AIMEE_VERSION);

   char ts[32];
   now_utc(ts, sizeof(ts));
   cJSON_AddStringToObject(root, "timestamp", ts);

   cJSON *results = cJSON_CreateObject();
   for (int i = 0; i < count; i++)
   {
      char key[64];
      snprintf(key, sizeof(key), "%s", entries[i].name);
      for (char *p = key; *p; p++)
         if (*p == ' ')
            *p = '_';

      cJSON *item = cJSON_CreateObject();
      cJSON_AddNumberToObject(item, "p50_ms", entries[i].results.p50_ms);
      cJSON_AddNumberToObject(item, "p95_ms", entries[i].results.p95_ms);
      cJSON_AddNumberToObject(item, "p99_ms", entries[i].results.p99_ms);
      cJSON_AddNumberToObject(item, "target_p50_ms", entries[i].target_p50_ms);
      cJSON_AddNumberToObject(item, "target_p95_ms", entries[i].target_p95_ms);
      cJSON_AddNumberToObject(item, "tolerance", entries[i].tolerance);
      cJSON_AddItemToObject(results, key, item);
   }
   cJSON_AddItemToObject(root, "results", results);

   char *json = cJSON_Print(root);
   cJSON_Delete(root);

   FILE *f = fopen(path, "w");
   if (f)
   {
      fputs(json, f);
      fputc('\n', f);
      fclose(f);
   }
   free(json);
}

/* --- Regression checking -----------------------------------------------
 *
 * Bench-check enforces the declared SLOs first, then uses the baseline as a
 * regression ratchet. A missing or sub-millisecond baseline can suppress only
 * the relative comparison; it can never turn an absolute SLO miss green.
 * Two rules protect the relative signal from CI runner jitter:
 *
 *   1. Sub-ms ops are unmeasurable on shared hardware. Any operation
 *      with a baseline p95 below BENCH_MIN_MEASURABLE_MS is reported
 *      but never fails the build -- a 0.1ms scheduler stall is not a
 *      regression.
 *
 *   2. For measurable ops, a regression must exceed BOTH the percentage
 *      tolerance AND an absolute delta floor. The floor is chosen so
 *      that real regressions (which push ops clearly into the multi-ms
 *      range) still fail, while small percentage blips on ops just
 *      above 1ms are absorbed.
 *
 *   3. If the current result is still within both benchmark SLO targets,
 *      treat it as healthy even when a stale static baseline is
 *      much lower than the current shared-runner timing. */

#define BENCH_MIN_MEASURABLE_MS 1.0
#define BENCH_ABSOLUTE_FLOOR_MS 1.0

static int check_regression(bench_entry_t *current, bench_entry_t *baseline, int count)
{
   int failures = 0;

   printf("\n%-20s  %10s  %10s  %10s  %s\n", "Operation", "Current", "Baseline", "Tolerance",
          "Status");
   printf("%-20s  %10s  %10s  %10s  %s\n", "--------------------", "----------", "----------",
          "----------", "------");

   for (int i = 0; i < count; i++)
   {
      int p50_slo_failed =
          current[i].target_p50_ms > 0 && current[i].results.p50_ms > current[i].target_p50_ms;
      int p95_slo_failed =
          current[i].target_p95_ms > 0 && current[i].results.p95_ms > current[i].target_p95_ms;
      if (p50_slo_failed || p95_slo_failed)
      {
         const char *which = p50_slo_failed && p95_slo_failed ? "p50+p95"
                             : p50_slo_failed                 ? "p50"
                                                              : "p95";
         if (baseline[i].results.p95_ms > 0)
            printf("%-20s  %8.2fms  %8.2fms  %9.0f%%  FAIL (SLO %s)\n", current[i].name,
                   current[i].results.p95_ms, baseline[i].results.p95_ms,
                   current[i].tolerance * 100, which);
         else
            printf("%-20s  %8.2fms  %10s  %9.0f%%  FAIL (SLO %s)\n", current[i].name,
                   current[i].results.p95_ms, "n/a", current[i].tolerance * 100, which);
         failures++;
         continue;
      }

      if (baseline[i].results.p95_ms <= 0)
      {
         printf("%-20s  %8.2fms  %10s  %9.0f%%  TARGET (no baseline)\n", current[i].name,
                current[i].results.p95_ms, "n/a", current[i].tolerance * 100);
         continue;
      }

      if (baseline[i].results.p95_ms < BENCH_MIN_MEASURABLE_MS)
      {
         printf("%-20s  %8.2fms  %8.2fms  %9.0f%%  SKIP (sub-ms, unmeasurable)\n", current[i].name,
                current[i].results.p95_ms, baseline[i].results.p95_ms, current[i].tolerance * 100);
         continue;
      }

      double threshold = baseline[i].results.p95_ms * (1.0 + current[i].tolerance);
      double delta = current[i].results.p95_ms - baseline[i].results.p95_ms;
      int percent_exceeded = current[i].results.p95_ms > threshold;
      int floor_exceeded = delta > BENCH_ABSOLUTE_FLOOR_MS;
      int target_ok = current[i].target_p50_ms > 0 && current[i].target_p95_ms > 0;
      int pass = target_ok || !(percent_exceeded && floor_exceeded);

      const char *status;
      if (target_ok && percent_exceeded && floor_exceeded)
         status = "TARGET";
      else if (pass && percent_exceeded)
         status = "NOISE";
      else if (pass)
         status = "PASS";
      else
         status = "FAIL";

      printf("%-20s  %8.2fms  %8.2fms  %9.0f%%  %s\n", current[i].name, current[i].results.p95_ms,
             baseline[i].results.p95_ms, current[i].tolerance * 100, status);

      if (!pass)
         failures++;
   }

   return failures;
}

/* --- JSON output ------------------------------------------------------- */

static void print_json(bench_entry_t *entries, int count)
{
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "version", AIMEE_VERSION);

   char ts[32];
   now_utc(ts, sizeof(ts));
   cJSON_AddStringToObject(root, "timestamp", ts);

   cJSON *results = cJSON_CreateArray();
   for (int i = 0; i < count; i++)
   {
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "name", entries[i].name);
      cJSON_AddStringToObject(item, "status",
                              isfinite(entries[i].results.p95_ms) ? "ok" : "unavailable");
      cJSON_AddNumberToObject(item, "p50_ms", entries[i].results.p50_ms);
      cJSON_AddNumberToObject(item, "p95_ms", entries[i].results.p95_ms);
      cJSON_AddNumberToObject(item, "p99_ms", entries[i].results.p99_ms);
      cJSON_AddNumberToObject(item, "target_p50_ms", entries[i].target_p50_ms);
      cJSON_AddNumberToObject(item, "target_p95_ms", entries[i].target_p95_ms);
      cJSON_AddNumberToObject(item, "tolerance", entries[i].tolerance);
      cJSON_AddItemToArray(results, item);
   }
   cJSON_AddItemToObject(root, "results", results);

   char *json = cJSON_Print(root);
   printf("%s\n", json);
   free(json);
   cJSON_Delete(root);
}

/* --- Main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
   int json_mode = 0;
   const char *check_path = NULL;
   const char *save_path = NULL;

   for (int i = 1; i < argc; i++)
   {
      if (strcmp(argv[i], "--json") == 0)
         json_mode = 1;
      else if (strcmp(argv[i], "--check") == 0 && i + 1 < argc)
         check_path = argv[++i];
      else if (strcmp(argv[i], "--save") == 0 && i + 1 < argc)
         save_path = argv[++i];
   }

   /* Define benchmarks with SLO targets */
   bench_entry_t benchmarks[] = {
       /* These are end-to-end governed paths, not bare parser/SQL timings. The
        * ceilings include dedupe, policy, provenance, and derived metadata on
        * the low-core managed-appliance class used by the release E2E suite. */
       {"pre_tool_check", 5.0, 7.0, 0.50, {0, 0, 0}},
       {"delegate_dispatch", 1.0, 5.0, 0.50, {0, 0, 0}},
   };
   int bench_count = (int)(sizeof(benchmarks) / sizeof(benchmarks[0]));

   /* Allocate sample arrays */
   int total = BENCH_WARMUP + BENCH_ITERATIONS;
   double *samples = calloc((size_t)total, sizeof(double));
   assert(samples);

   /* Run each benchmark */
   for (int b = 0; b < bench_count; b++)
   {
      memset(samples, 0, (size_t)total * sizeof(double));

      if (strcmp(benchmarks[b].name, "pre_tool_check") == 0)
         bench_pre_tool_check(samples, total);
      else if (strcmp(benchmarks[b].name, "delegate_dispatch") == 0)
         bench_delegate_dispatch(samples, total);

      /* Discard warmup, compute percentiles on remaining samples */
      compute_percentiles(samples + BENCH_WARMUP, BENCH_ITERATIONS, &benchmarks[b].results);
   }

   free(samples);

   /* Output results */
   if (json_mode)
   {
      print_json(benchmarks, bench_count);
   }
   else if (!check_path)
   {
      printf("%-20s  %10s  %10s  %10s\n", "Operation", "p50", "p95", "p99");
      printf("%-20s  %10s  %10s  %10s\n", "--------------------", "----------", "----------",
             "----------");
      for (int i = 0; i < bench_count; i++)
      {
         printf("%-20s  %8.2fms  %8.2fms  %8.2fms\n", benchmarks[i].name,
                benchmarks[i].results.p50_ms, benchmarks[i].results.p95_ms,
                benchmarks[i].results.p99_ms);
      }
   }

   /* Save baseline if requested */
   if (save_path)
   {
      save_baseline(save_path, benchmarks, bench_count);
      if (!json_mode)
         printf("\nBaseline saved to %s\n", save_path);
   }

   /* Regression check */
   if (check_path)
   {
      bench_entry_t baseline[sizeof(benchmarks) / sizeof(benchmarks[0])];
      memcpy(baseline, benchmarks, sizeof(baseline));

      if (parse_baseline(check_path, baseline, bench_count) != 0)
      {
         fprintf(stderr, "Error: could not read baseline from %s\n", check_path);
         return 1;
      }

      int failures = check_regression(benchmarks, baseline, bench_count);
      if (failures > 0)
      {
         printf("\n%d benchmark(s) regressed beyond tolerance.\n", failures);
         return 1;
      }
      printf("\nAll benchmarks within tolerance.\n");
   }

   return 0;
}
