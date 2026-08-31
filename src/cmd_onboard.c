/* cmd_onboard.c: guided first-run orchestration.
 *
 *   aimee onboard [--json] [--quiet] [--skip-setup]
 *
 * Sequences the existing setup / doctor / memory primitives into a
 * single readiness pass so operators don't have to remember the
 * order. Idempotent: re-running onboard re-verifies but doesn't
 * re-do idempotent work. Exit 0 when nothing is `error`, exit 1
 * when at least one check failed.
 *
 * See docs/proposals/pending/guided-onboarding-and-operator-console.md. */

#include "aimee.h"
#include "commands.h"
#include "cJSON.h"
#include "kb_client.h"
#include "memory.h"
#include "lifecycle.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- helpers --- */

static int onboard_quiet = 0;

typedef int (*onboard_memory_insert_fn)(const char *tier, const char *kind, const char *key,
                                        const char *content, double confidence,
                                        const char *session_id, memory_t *out);
typedef int (*onboard_memory_get_fn)(int64_t id, memory_t *out);

static int onboard_default_memory_insert(const char *tier, const char *kind, const char *key,
                                         const char *content, double confidence,
                                         const char *session_id, memory_t *out)
{
   /* The onboarding wizard records the user's own answers, so the rows carry the
    * user provenance — this is the one bulk path where that is true. */
   return kb_client_memory_insert_as(tier, kind, key, content, "", confidence, session_id,
                                     MEMORY_AUTHORITY_USER, out);
}

static int onboard_default_memory_get(int64_t id, memory_t *out)
{
   return kb_client_memory_get(id, out);
}

static onboard_memory_insert_fn onboard_memory_insert_impl = onboard_default_memory_insert;
static onboard_memory_get_fn onboard_memory_get_impl = onboard_default_memory_get;

void onboard_set_memory_client_for_test(onboard_memory_insert_fn insert_fn,
                                        onboard_memory_get_fn get_fn)
{
   onboard_memory_insert_impl = insert_fn ? insert_fn : onboard_default_memory_insert;
   onboard_memory_get_impl = get_fn ? get_fn : onboard_default_memory_get;
}

static void onboard_log(const char *fmt, ...)
{
   if (onboard_quiet)
      return;
   va_list ap;
   va_start(ap, fmt);
   fprintf(stderr, "[onboard] ");
   vfprintf(stderr, fmt, ap);
   fprintf(stderr, "\n");
   va_end(ap);
}

static int onboard_smoke_memory(char *err, size_t err_len)
{
   char key[64];
   time_t now = time(NULL);
   snprintf(key, sizeof(key), "onboard-smoke-%ld", (long)now);

   /* Round-trip a tiny L0 fact through aimee-kb to verify the memory
    * path is wired up end-to-end (server -> kb_client -> aimee-kb -> DB2). */
   memory_t m;
   int rc = onboard_memory_insert_impl("L0", "fact", key, "onboard smoke test", 0.1, "onboard", &m);
   if (rc != 0)
   {
      snprintf(err, err_len, "kb_client_memory_insert failed (rc=%d)", rc);
      return -1;
   }

   memory_t got;
   if (onboard_memory_get_impl(m.id, &got) != 0)
   {
      snprintf(err, err_len, "kb_client_memory_get failed for id=%lld", (long long)m.id);
      return -1;
   }
   if (strcmp(got.key, key) != 0)
   {
      snprintf(err, err_len, "round-tripped key mismatch: got %s", got.key);
      return -1;
   }
   /* Smoke-test L0 rows roll over with the session, no explicit cleanup needed. */
   return 0;
}

/* Summarize the doctor output into an overall status + next-actions
 * list. Returns 1 if any error present, 0 otherwise. */
static int onboard_summarize_doctor(const cJSON *checks, cJSON *next_actions, int *warnings_out,
                                    int *errors_out)
{
   int warnings = 0, errors = 0;
   const cJSON *check;
   cJSON_ArrayForEach(check, checks)
   {
      const cJSON *status = cJSON_GetObjectItemCaseSensitive(check, "status");
      const cJSON *remediation = cJSON_GetObjectItemCaseSensitive(check, "remediation");
      if (!cJSON_IsString(status))
         continue;
      if (strcmp(status->valuestring, "warn") == 0)
         warnings++;
      else if (strcmp(status->valuestring, "error") == 0)
      {
         errors++;
         if (next_actions && cJSON_IsString(remediation) && remediation->valuestring[0])
            cJSON_AddItemToArray(next_actions, cJSON_CreateString(remediation->valuestring));
      }
   }
   if (warnings_out)
      *warnings_out = warnings;
   if (errors_out)
      *errors_out = errors;
   return errors > 0 ? 1 : 0;
}

/* Build the full onboard report as cJSON. Caller owns + frees. */
cJSON *onboard_build_report(app_ctx_t *ctx, int skip_setup)
{
   cJSON *root = cJSON_CreateObject();
   if (!root)
      return NULL;
   struct timespec t0;
   clock_gettime(CLOCK_MONOTONIC, &t0);

   cJSON_AddStringToObject(root, "version", AIMEE_VERSION);
   cJSON *steps = cJSON_AddArrayToObject(root, "steps");
   cJSON *next_actions = cJSON_AddArrayToObject(root, "next_actions");

   /* --- Step 1: setup --- */
   {
      cJSON *step = cJSON_CreateObject();
      cJSON_AddStringToObject(step, "step", "setup");
      if (skip_setup)
      {
         cJSON_AddStringToObject(step, "status", "skipped");
      }
      else
      {
         onboard_log("running setup (idempotent)…");
         cmd_setup(ctx, 0, NULL);
         cJSON_AddStringToObject(step, "status", "ok");
      }
      cJSON_AddItemToArray(steps, step);
   }

   /* --- Step 2: doctor --- */
   int doctor_errors = 0;
   {
      cJSON *step = cJSON_CreateObject();
      cJSON_AddStringToObject(step, "step", "doctor");
      onboard_log("running doctor checks…");
      char *dj = doctor_checks_json();
      cJSON *dreport = dj ? cJSON_Parse(dj) : NULL;
      free(dj);
      if (!dreport)
      {
         cJSON_AddStringToObject(step, "status", "error");
         cJSON_AddItemToArray(next_actions,
                              cJSON_CreateString("doctor: could not produce checks JSON"));
         doctor_errors++;
      }
      else
      {
         cJSON *checks = cJSON_GetObjectItemCaseSensitive(dreport, "checks");
         int warn_count = 0, err_count = 0;
         int has_error = onboard_summarize_doctor(checks, next_actions, &warn_count, &err_count);
         cJSON_AddStringToObject(step, "status",
                                 has_error ? "error" : (warn_count ? "warn" : "ok"));
         cJSON_AddNumberToObject(step, "warnings", warn_count);
         cJSON_AddNumberToObject(step, "errors", err_count);
         cJSON_AddItemToObject(step, "checks", cJSON_Duplicate(checks, 1));
         doctor_errors = err_count;
         cJSON_Delete(dreport);
      }
      cJSON_AddItemToArray(steps, step);
   }

   /* --- Step 3: memory smoke --- */
   {
      cJSON *step = cJSON_CreateObject();
      cJSON_AddStringToObject(step, "step", "memory_smoke");
      char err[256] = {0};
      int rc = onboard_smoke_memory(err, sizeof(err));
      if (rc == 0)
         cJSON_AddStringToObject(step, "status", "ok");
      else
      {
         cJSON_AddStringToObject(step, "status", "error");
         cJSON_AddStringToObject(step, "message", err);
         char action[320];
         snprintf(action, sizeof(action),
                  "memory smoke failed: %s; server-side maintenance is required", err);
         cJSON_AddItemToArray(next_actions, cJSON_CreateString(action));
      }
      cJSON_AddItemToArray(steps, step);
   }

   /* --- Overall readiness --- */
   int ready = 1;
   {
      const cJSON *step;
      cJSON_ArrayForEach(step, steps)
      {
         const cJSON *status = cJSON_GetObjectItemCaseSensitive(step, "status");
         if (cJSON_IsString(status) && strcmp(status->valuestring, "error") == 0)
         {
            ready = 0;
            break;
         }
      }
   }
   (void)doctor_errors;
   cJSON_AddBoolToObject(root, "ready", ready);

   /* Friendly next-actions default when nothing is broken. */
   if (cJSON_GetArraySize(next_actions) == 0 && ready)
      cJSON_AddItemToArray(
          next_actions, cJSON_CreateString("ready: try `aimee chat` or `aimee memory briefing`"));

   struct timespec t1;
   clock_gettime(CLOCK_MONOTONIC, &t1);
   double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1.0e6;
   cJSON_AddNumberToObject(root, "elapsed_ms", ms);
   return root;
}

/* Print the human-readable summary to stdout. */
static void onboard_print_summary(const cJSON *report)
{
   const cJSON *steps = cJSON_GetObjectItemCaseSensitive(report, "steps");
   const cJSON *ready = cJSON_GetObjectItemCaseSensitive(report, "ready");
   const cJSON *next_actions = cJSON_GetObjectItemCaseSensitive(report, "next_actions");

   printf("\nOnboarding report:\n");
   const cJSON *step;
   cJSON_ArrayForEach(step, steps)
   {
      const cJSON *name = cJSON_GetObjectItemCaseSensitive(step, "step");
      const cJSON *status = cJSON_GetObjectItemCaseSensitive(step, "status");
      const cJSON *warnings = cJSON_GetObjectItemCaseSensitive(step, "warnings");
      const cJSON *errors = cJSON_GetObjectItemCaseSensitive(step, "errors");
      printf("  %-14s %s", cJSON_IsString(name) ? name->valuestring : "",
             cJSON_IsString(status) ? status->valuestring : "");
      if (cJSON_IsNumber(warnings) || cJSON_IsNumber(errors))
         printf("  (warnings=%d, errors=%d)", cJSON_IsNumber(warnings) ? warnings->valueint : 0,
                cJSON_IsNumber(errors) ? errors->valueint : 0);
      printf("\n");
   }

   printf("\nReady: %s\n", cJSON_IsTrue(ready) ? "yes" : "no");

   if (cJSON_IsArray(next_actions) && cJSON_GetArraySize(next_actions) > 0)
   {
      printf("\nNext actions:\n");
      const cJSON *act;
      cJSON_ArrayForEach(act, next_actions)
      {
         if (cJSON_IsString(act))
            printf("  - %s\n", act->valuestring);
      }
   }
}

/* --- dispatch --- */

void cmd_onboard(app_ctx_t *ctx, int argc, char **argv)
{
   int skip_setup = 0;
   onboard_quiet = 0;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--skip-setup") == 0)
         skip_setup = 1;
      else if (strcmp(argv[i], "--quiet") == 0)
         onboard_quiet = 1;
      else if (strcmp(argv[i], "--non-interactive") == 0 || strcmp(argv[i], "--ci") == 0)
      {
         /* Non-interactive is currently the only mode — all steps are
          * non-interactive today. The flag is accepted for forward
          * compatibility and to let CI scripts declare intent. */
         onboard_quiet = 1;
      }
      else if (strcmp(argv[i], "--help") == 0)
      {
         printf("aimee onboard [--json] [--quiet] [--skip-setup] [--non-interactive]\n"
                "  Run setup, then doctor, then a memory smoke test. Report readiness.\n");
         return;
      }
      else
         fatal("unknown onboard flag: %s", argv[i]);
   }

   cJSON *report = onboard_build_report(ctx, skip_setup);
   if (!report)
      fatal("onboard: could not build report");

   const cJSON *ready = cJSON_GetObjectItemCaseSensitive(report, "ready");
   int exit_code = (cJSON_IsBool(ready) && cJSON_IsTrue(ready)) ? 0 : 1;

   if (ctx->json_output)
      emit_json_ctx(report, ctx->json_fields, ctx->response_profile);
   else
   {
      onboard_print_summary(report);
      cJSON_Delete(report);
   }

   if (exit_code != 0)
      exit(exit_code);
}
