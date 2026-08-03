/* cli_kb_smoke.c: `aimee kb smoke` — is the kb this client talks to actually usable?
 *
 * scripts/aimee-kb-docker-smoke.sh answers the same question for a container the
 * operator has just built, and stays the right tool for that: it owns compose, and
 * it can `docker compose exec` into the kb to reach the bundled embedder and the
 * bundled synthesis on loopback, which nothing outside the container can see.
 *
 * It is the wrong tool for a deployment. It needs docker, compose, a checkout, curl
 * on the host and buildx >= 0.17 for its --up path. A thin client has none of those,
 * and the host that has them is not necessarily the host running the kb.
 *
 * This runs client-side rather than as a kb.smoke server method on purpose. The
 * question an operator is asking is "can I reach a working kb from here", so the
 * checks have to traverse the same client -> server -> kb path everything else
 * uses. A server-side method would answer a different question, and would answer it
 * from inside the thing under test.
 *
 * WHAT THIS CANNOT SEE is in the --help text rather than left implicit: a bundled
 * embedder or synthesis model listens on loopback INSIDE the kb container, so no
 * client can prove it started, and synthesis provider state is not carried on
 * kb.health at all. Only the docker script can reach those. The two overlap, and
 * neither replaces the other.
 */
#include "cli_kb_smoke.h"
#include "cli_client.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
   int passed;
   int failed;
   int skipped;
   int json_out;
   cJSON *rows;
} smoke_t;

static const char *jstr(cJSON *o, const char *k)
{
   cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
   return (v && cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
}

static int jbool(cJSON *o, const char *k, int fallback)
{
   cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
   if (cJSON_IsBool(v))
      return cJSON_IsTrue(v) ? 1 : 0;
   return fallback;
}

/* Three outcomes, and "skip" is load-bearing rather than a gentler failure. An
 * un-embedded corpus is a supported deployment; reporting it as a failure would
 * teach operators to ignore this command, which is how a check stops being worth
 * running. */
static void report(smoke_t *s, const char *name, int ok, int skip, const char *detail)
{
   if (skip)
      s->skipped++;
   else if (ok)
      s->passed++;
   else
      s->failed++;

   cJSON *row = cJSON_CreateObject();
   cJSON_AddStringToObject(row, "check", name);
   cJSON_AddStringToObject(row, "outcome", skip ? "skip" : (ok ? "pass" : "fail"));
   if (detail && detail[0])
      cJSON_AddStringToObject(row, "detail", detail);
   cJSON_AddItemToArray(s->rows, row);
}

/* Rendering is separate from evaluation so the checks stay pure functions of the
 * payload, which is what makes them testable without a server. */
static void render_rows(cJSON *rows)
{
   cJSON *row = NULL;
   cJSON_ArrayForEach(row, rows)
   {
      const char *outcome = jstr(row, "outcome");
      const char *tag = strcmp(outcome, "skip") == 0   ? "SKIP"
                        : strcmp(outcome, "pass") == 0 ? "PASS"
                                                       : "FAIL";
      const char *detail = jstr(row, "detail");
      if (detail[0])
         printf("  %-4s  %-26s %s\n", tag, jstr(row, "check"), detail);
      else
         printf("  %-4s  %s\n", tag, jstr(row, "check"));
   }
}

static cJSON *call(const char *method, int timeout_ms)
{
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return NULL;
   cJSON_AddStringToObject(req, "method", method);
   cJSON *resp = cli_v1_dispatch(req, timeout_ms);
   cJSON_Delete(req);
   return resp;
}

static void check_vectors(smoke_t *s, cJSON *h)
{
   cJSON *v = h ? cJSON_GetObjectItemCaseSensitive(h, "vector") : NULL;
   if (!v)
   {
      report(s, "vector store ready", 0, 0, "no vector block in kb health");
      return;
   }
   int ok = jbool(v, "available", 0) && jbool(v, "kb_collection_ready", 0) &&
            jbool(v, "memory_collection_ready", 0);
   const char *backend = jstr(v, "backend");
   cJSON *kbp = cJSON_GetObjectItemCaseSensitive(v, "kb_points");
   cJSON *memp = cJSON_GetObjectItemCaseSensitive(v, "memory_points");
   char detail[192];
   snprintf(detail, sizeof(detail), "%s, %d kb / %d memory point(s)",
            backend[0] ? backend : "backend unreported",
            cJSON_IsNumber(kbp) ? (int)kbp->valuedouble : 0,
            cJSON_IsNumber(memp) ? (int)memp->valuedouble : 0);
   report(s, "vector store ready", ok, 0, detail);
}

/* Zero embeddings is not a failure: "external embedder only" is supported, and the
 * builtin lexical embedder serves until one is selected. It is reported because a
 * deployment that MEANT to embed looks identical from outside, which is how a box
 * ends up serving lexical-only search behind a healthy banner. */
static void check_embeddings(smoke_t *s, cJSON *h)
{
   if (!h)
      return;
   cJSON *e = cJSON_GetObjectItemCaseSensitive(h, "embeddings");
   int n = cJSON_IsNumber(e) ? (int)e->valuedouble : 0;
   char detail[128];
   if (n > 0)
   {
      snprintf(detail, sizeof(detail), "%d embedding(s)", n);
      report(s, "corpus is embedded", 1, 0, detail);
   }
   else
   {
      report(s, "corpus is embedded", 0, 1, "0 embeddings; lexical search only");
   }
}

/* An embedder of the wrong width cannot store a single vector, and the rest of health
 * reads perfectly normal while that is true.
 *
 * Measured: booting the nomic kb image (768) over a store recorded at 384 left this
 * command reporting 5 passed, 0 failed. The kb was up, the vector store was "ready",
 * search "answered" (with nothing, forever), and no check looked at the one field that
 * said the embedder and the store disagreed.
 *
 * The kb publishes it in `warnings` now, so read them rather than adding a second
 * opinion here: the kb knows both widths and this command does not. Any warning is
 * surfaced, because a warning the kb bothered to raise and nothing prints is the shape
 * of every bug this command exists to catch. */
static void check_warnings(smoke_t *s, cJSON *h)
{
   cJSON *w = h ? cJSON_GetObjectItemCaseSensitive(h, "warnings") : NULL;
   if (!cJSON_IsArray(w) || cJSON_GetArraySize(w) == 0)
      return;
   cJSON *first = cJSON_GetArrayItem(w, 0);
   char detail[256];
   int n = cJSON_GetArraySize(w);
   snprintf(detail, sizeof(detail), "%s%s", cJSON_IsString(first) ? first->valuestring : "warning",
            n > 1 ? " (and more)" : "");
   report(s, "kb reports no warnings", 0, 0, detail);
}

/* The check that earns this command its keep. A queue where everything has failed
 * reports `status: ok` everywhere else, because the kb is up and answering: the
 * failures are in work it already gave up on. Nothing else surfaces it. */
static void check_queue(smoke_t *s, cJSON *h)
{
   cJSON *q = h ? cJSON_GetObjectItemCaseSensitive(h, "queue") : NULL;
   if (!q)
      return;
   cJSON *fj = cJSON_GetObjectItemCaseSensitive(q, "failed");
   cJSON *tj = cJSON_GetObjectItemCaseSensitive(q, "total");
   int failed = cJSON_IsNumber(fj) ? (int)fj->valuedouble : 0;
   int total = cJSON_IsNumber(tj) ? (int)tj->valuedouble : 0;
   char detail[160];

   if (total == 0)
   {
      report(s, "ingest queue healthy", 0, 1, "no jobs recorded");
      return;
   }
   snprintf(detail, sizeof(detail), "%d of %d job(s) failed", failed, total);
   report(s, "ingest queue healthy", failed < total, 0, detail);
}

/* The only check that drives the retrieval stack rather than reading a flag. An
 * empty result is a pass: a kb with no corpus still has a working query path, and
 * failing on emptiness would make this useless on a fresh deployment, which is
 * exactly when someone runs it. */
static void check_search(smoke_t *s)
{
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return;
   cJSON_AddStringToObject(req, "method", "kb.search");
   cJSON_AddStringToObject(req, "query", "aimee kb smoke probe");
   cJSON_AddNumberToObject(req, "max", 1);
   cJSON *resp = cli_v1_dispatch(req, 60000);
   cJSON_Delete(req);

   if (!resp)
   {
      report(s, "search path answers", 0, 0, "no response");
      return;
   }
   const char *err = jstr(resp, "error");
   report(s, "search path answers", err[0] ? 0 : 1, 0, err[0] ? err : "query accepted");
   cJSON_Delete(resp);
}

/* Evaluate every check derivable from a kb.health payload. Pure: no I/O, no
 * network. Exposed so a test can feed canned payloads, because the failure this
 * command already had was reading kb_health_t's field names out of a JSON response
 * that does not use them, and that produced a confident FAIL on a healthy kb. */
cJSON *cli_kb_smoke_eval_health(cJSON *health, int *passed, int *failed, int *skipped)
{
   smoke_t s;
   memset(&s, 0, sizeof(s));
   s.rows = cJSON_CreateArray();

   if (!health)
   {
      report(&s, "kb is reachable", 0, 0, "no response over /v1");
   }
   else
   {
      const char *err = jstr(health, "error");
      if (err[0])
      {
         report(&s, "kb is reachable", 0, 0, err);
      }
      else
      {
         const char *summary = jstr(health, "summary_status");
         report(&s, "kb is reachable", jbool(health, "available", 0), 0,
                summary[0] ? summary : NULL);
         check_vectors(&s, health);
         check_embeddings(&s, health);
         check_queue(&s, health);
         check_warnings(&s, health);
      }
   }

   if (passed)
      *passed = s.passed;
   if (failed)
      *failed = s.failed;
   if (skipped)
      *skipped = s.skipped;
   return s.rows;
}

static void usage(void)
{
   printf("Usage: aimee kb smoke [--json]\n\n"
          "Exercise the kb this client is pointed at: reachability, the vector\n"
          "store, whether the corpus is embedded, the ingest queue, and the search\n"
          "path. Every check traverses the same client -> server -> kb path as a\n"
          "real command, so it works against a local, remote or mTLS kb with\n"
          "nothing installed.\n\n"
          "Exits non-zero if any check FAILS. A SKIP is not a failure: a kb with no\n"
          "corpus, or one using an external embedder, is a supported deployment.\n\n"
          "Not covered, because no client can see it: a bundled embedder or bundled\n"
          "synthesis model listens on loopback inside the kb container, and\n"
          "synthesis provider state is not carried on kb.health. To prove those\n"
          "started, use scripts/aimee-kb-docker-smoke.sh, which execs into the\n"
          "container.\n");
}

int cli_kb_smoke(int argc, char **argv, int json_output)
{
   int json_out = json_output;
   for (int i = 0; i < argc; i++)
   {
      if (!argv[i])
         continue;
      if (strcmp(argv[i], "--json") == 0)
         json_out = 1;
      else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
      {
         usage();
         return 0;
      }
   }

   cJSON *health = call("kb.health", 15000);
   int passed = 0, failed = 0, skipped = 0;
   cJSON *rows = cli_kb_smoke_eval_health(health, &passed, &failed, &skipped);
   if (health)
      cJSON_Delete(health);

   /* Search is the one check that cannot come from the health payload: it drives
    * the retrieval stack rather than reading a flag. */
   smoke_t s;
   memset(&s, 0, sizeof(s));
   s.rows = rows;
   s.passed = passed;
   s.failed = failed;
   s.skipped = skipped;
   check_search(&s);

   if (!json_out)
   {
      printf("Exercising the kb\n");
      render_rows(s.rows);
      printf("\nSummary: %d passed, %d failed, %d skipped\n", s.passed, s.failed, s.skipped);
      cJSON_Delete(s.rows);
   }
   else
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "status", s.failed ? "fail" : "ok");
      cJSON_AddNumberToObject(obj, "passed", s.passed);
      cJSON_AddNumberToObject(obj, "failed", s.failed);
      cJSON_AddNumberToObject(obj, "skipped", s.skipped);
      cJSON_AddItemToObject(obj, "checks", s.rows);
      char *j = cJSON_Print(obj);
      cJSON_Delete(obj);
      if (j)
      {
         puts(j);
         free(j);
      }
   }
   return s.failed ? 1 : 0;
}
