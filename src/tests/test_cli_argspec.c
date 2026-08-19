/* test_cli_argspec.c: a served argument spec must build the SAME body the
 * compiled marshaller does.
 *
 * This is the only property that makes moving argument handling server-side
 * safe. The manifest can already add a command, route it and document it; the
 * spec lets it describe the command's arguments too. But every one of these
 * methods works today through a hand-written marshaller, and a spec that is
 * merely plausible would change what the CLI sends for commands people already
 * run — silently, because both bodies are valid JSON and the server answers
 * either.
 *
 * So the test is differential, not descriptive: for each specced method it runs
 * the real marshaller and the interpreter over the same argv and compares the
 * rendered JSON. A spec is allowed to ship only once it is indistinguishable.
 *
 * The samples per method are deliberately awkward — flag absent, flag present,
 * positional instead of flag, empty string, unknown extra flag — because the
 * cases that differ are the edges, and a spec that only agrees on the happy
 * path is a spec that breaks the first operator who omits an argument.
 */
#include "cli_argspec.h"
#include "cli_v1_routes_internal.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;

static void fail(const char *what, const char *detail)
{
   g_fail++;
   printf("FAIL %s: %s\n", what, detail ? detail : "");
}

/* ---- the SHIPPED specs, and the samples that prove them ----------------- */

/* The same rows the server serves. Included, not copied: a test with its own
 * specs would prove specs that are not the ones shipped. */
typedef struct
{
   const char *method;
   const char *spec;
} argspec_row_t;

static const argspec_row_t SHIPPED[] = {
#include "../server/cli_argspec_defs_data.h"
};

/* Samples are deliberately awkward — flag absent, flag present, positional
 * instead of flag, empty string, unknown extra flag — because the cases that
 * differ are the edges. A spec that only agrees on the happy path is a spec
 * that breaks the first operator who omits an argument. */
typedef struct
{
   const char *method;
   const char *argv[8];
} sample_t;

static const sample_t SAMPLES[] = {
    /* provider.list — every bool flag absent, then each present, then all.
     * An unknown flag must not change the body: the server rejects what it
     * does not know, and the client inventing a field for it would be worse. */
    {"provider.list", {NULL}},
    {"provider.list", {"--json", NULL}},
    {"provider.list", {"--available", NULL}},
    {"provider.list", {"--all", "--json", NULL}},
    {"provider.list", {"--nonsense", NULL}},

    /* provider.models — a positional, with and without the bool. An empty
     * positional is not a value: both sides must drop it, or one sends
     * "name":"" and the server filters on the empty string. */
    {"provider.models", {NULL}},
    {"provider.models", {"openai", NULL}},
    {"provider.models", {"openai", "--json", NULL}},
    {"provider.models", {"", NULL}},

    /* catalog.list — a valued flag, a true_if_set and an explicit bool, which
     * render differently (absent vs true vs false) and are the likeliest thing
     * for a spec to get subtly wrong. */
    {"catalog.list", {NULL}},
    {"catalog.list", {"--json", NULL}},
    {"catalog.list", {"--open-weights", NULL}},
    {"catalog.list", {"--capability", "vision", NULL}},
    {"catalog.list", {"--capability", "vision", "--json", "--open-weights", NULL}},

    /* trigger.list — a single optional flag. */
    {"trigger.list", {NULL}},
    {"trigger.list", {"--status", "pending", NULL}},

    /* trigger.status / trigger.cancel — required, reachable as a positional OR
     * --id, and both sides must REFUSE when it is absent rather than send a
     * body without it. */
    {"trigger.status", {"tr-1", NULL}},
    {"trigger.status", {"--id", "tr-1", NULL}},
    {"trigger.status", {NULL}},
    {"trigger.cancel", {"tr-1", NULL}},
    {"trigger.cancel", {"--id", "tr-1", NULL}},
    {"trigger.cancel", {NULL}},

    /* model.episodes — optional, positional OR --agent. */
    {"model.episodes", {NULL}},
    {"model.episodes", {"claude", NULL}},
    {"model.episodes", {"--agent", "claude", NULL}},
    {"model.episodes", {"", NULL}},

    /* graph.sync_code — a plain optional positional, which does NOT fall back
     * to a flag; sampling --project proves it stays absent rather than quietly
     * acquiring the fallback that positional_or_flag has. */
    {"graph.sync_code", {NULL}},
    {"graph.sync_code", {"aimee", NULL}},
    {"graph.sync_code", {"--project", "aimee", NULL}},

    /* dogfood.report — two optional valued flags, neither with a positional
     * fallback, so a bare positional must not become either field. */
    {"dogfood.report", {NULL}},
    {"dogfood.report", {"--month", "2026-08", NULL}},
    {"dogfood.report", {"--dir", "reports/aug", NULL}},
    {"dogfood.report", {"--month", "2026-08", "--dir", "reports/aug", NULL}},
    {"dogfood.report", {"stray-positional", NULL}},

    /* The "empty":"emit" family. Every one of these gates on pos_count alone,
     * so an empty argument is a value the operator typed and is sent. The
     * empty-string samples are the whole point: without the flag each of these
     * disagreed with its marshaller on exactly that input, and on nothing
     * else. */
    {"eval.results", {NULL}},
    {"eval.results", {"suite-a", NULL}},
    {"eval.results", {"", NULL}},

    {"cert.revoke", {NULL}},
    {"cert.revoke", {"AB12", NULL}},
    {"cert.revoke", {"", NULL}},

    {"vault.capability", {NULL}},
    {"vault.capability", {"grant", NULL}},
    {"vault.capability", {"grant", "uid:1000", NULL}},
    {"vault.capability", {"", "uid:1000", NULL}},

    {"vault.delete", {"git", "token", NULL}},
    {"vault.delete", {"git", "", NULL}},
    {"vault.delete", {NULL}},

    {"vault.set", {"git", "token", "s3cret", NULL}},
    {"vault.set", {"git", "token", "", NULL}},
    {"vault.set", {"git", NULL}},

    {"vault.set_server", {"git", "token", "s3cret", NULL}},
    {"vault.set_server", {"git", "", "s3cret", NULL}},
    {"vault.set_server", {NULL}},

    /* The lenient-number family. The non-numeric samples are the point: atoi
     * turns "abc" into 0 and "12x" into 12, and a spec claiming to describe
     * these has to do the same or it diverges on the first typo. */
    {"graph.explain", {NULL}},
    {"graph.explain", {"widget", NULL}},
    {"graph.explain", {"widget", "--limit", "5", NULL}},
    {"graph.explain", {"widget", "--limit", "abc", NULL}},
    {"graph.explain", {"widget", "--limit", "12x", NULL}},

    {"aux.test", {NULL}},
    {"aux.test", {"t", NULL}},
    {"aux.test", {"t", "p", NULL}},
    {"aux.test", {"t", "p", "512", NULL}},
    {"aux.test", {"t", "p", "abc", NULL}},
    {"aux.test", {"t", "", "7", NULL}},

    {"dogfood.review", {NULL}},
    {"dogfood.review", {"--month", "2026-08", NULL}},
    {"dogfood.review", {"--limit", "9", NULL}},
    {"dogfood.review", {"--limit", "nine", NULL}},
    {"dogfood.review", {"--json", NULL}},

    {"config.get", {NULL}},
    {"config.get", {"v0", NULL}},
    {"config.get", {"", NULL}},
    {"config.set", {NULL}},
    {"config.set", {"v0", "v1", NULL}},
    {"config.set", {"", "v1", NULL}},
    {"delegate.aggregate", {NULL}},
    {"delegate.aggregate", {"v0", NULL}},
    {"delegate.aggregate", {"", NULL}},
    {"evidence.fidelity_retrieval_event", {NULL}},
    {"evidence.fidelity_retrieval_event", {"v0", NULL}},
    {"evidence.fidelity_retrieval_event", {"", NULL}},
    {"evidence.provenance_retrieval_event", {NULL}},
    {"evidence.provenance_retrieval_event", {"v0", NULL}},
    {"evidence.provenance_retrieval_event", {"", NULL}},
    {"evidence.trace_retrieval_event", {NULL}},
    {"evidence.trace_retrieval_event", {"v0", NULL}},
    {"evidence.trace_retrieval_event", {"", NULL}},
    {"index.scan", {NULL}},
    {"index.scan", {"v0", "v1", NULL}},
    {"index.scan", {"", "v1", NULL}},
    {"index.scan", {"v0", "v1", "--force", NULL}},
    {"kb.build", {NULL}},
    {"kb.build", {"--path", "x", NULL}},
    {"kb.build", {"--path", "x", "--project", "x", NULL}},
    {"kb.build", {"--path", "x", "--force", NULL}},
    {"kb.build", {"--path", "x", "--embed", "x", NULL}},
    {"kb.ingest", {NULL}},
    {"kb.ingest", {"v0", NULL}},
    {"kb.ingest", {"", NULL}},
    {"kb.ingest", {"v0", "--force", NULL}},
    {"kb.ingest", {"v0", "--embed", "x", NULL}},
    {"kb.status", {NULL}},
    {"kb.status", {"v0", NULL}},
    {"kb.status", {"", NULL}},
    {"kb.update", {NULL}},
    {"kb.update", {"v0", "v1", NULL}},
    {"kb.update", {"", "v1", NULL}},
    {"kb.update", {"v0", "v1", "--embed", "x", NULL}},
    {"curator.implements", {NULL}},
    {"curator.implements", {"v0", NULL}},
    {"curator.implements", {"", NULL}},
    {"curator.synthesize", {NULL}},
    {"curator.synthesize", {"v0", NULL}},
    {"curator.synthesize", {"", NULL}},
    {"provider.quota", {NULL}},
    {"provider.quota", {"v0", NULL}},
    {"provider.quota", {"", NULL}},
    {"provider.show", {NULL}},
    {"provider.show", {"v0", NULL}},
    {"provider.show", {"", NULL}},
    {"provider.test", {NULL}},
    {"provider.test", {"v0", NULL}},
    {"provider.test", {"", NULL}},
    {"model.add", {NULL}},
    {"model.add", {"one", NULL}},
    {"model.add", {"one", "two", NULL}},
    {"model.add", {"--flag", "v", NULL}},
    {"model.add", {"", NULL}},
    {"model.disable", {NULL}},
    {"model.disable", {"one", NULL}},
    {"model.disable", {"one", "two", NULL}},
    {"model.disable", {"--flag", "v", NULL}},
    {"model.disable", {"", NULL}},
    {"model.enable", {NULL}},
    {"model.enable", {"one", NULL}},
    {"model.enable", {"one", "two", NULL}},
    {"model.enable", {"--flag", "v", NULL}},
    {"model.enable", {"", NULL}},
    {"model.episodes", {NULL}},
    {"model.episodes", {"one", NULL}},
    {"model.episodes", {"one", "two", NULL}},
    {"model.episodes", {"--flag", "v", NULL}},
    {"model.episodes", {"", NULL}},
    {"model.list", {NULL}},
    {"model.list", {"one", NULL}},
    {"model.list", {"one", "two", NULL}},
    {"model.list", {"--flag", "v", NULL}},
    {"model.list", {"", NULL}},
    {"model.local", {NULL}},
    {"model.local", {"one", NULL}},
    {"model.local", {"one", "two", NULL}},
    {"model.local", {"--flag", "v", NULL}},
    {"model.local", {"", NULL}},
    {"model.personas", {NULL}},
    {"model.personas", {"one", NULL}},
    {"model.personas", {"one", "two", NULL}},
    {"model.personas", {"--flag", "v", NULL}},
    {"model.personas", {"", NULL}},
    {"model.probe", {NULL}},
    {"model.probe", {"one", NULL}},
    {"model.probe", {"one", "two", NULL}},
    {"model.probe", {"--flag", "v", NULL}},
    {"model.probe", {"", NULL}},
    {"model.remove", {NULL}},
    {"model.remove", {"one", NULL}},
    {"model.remove", {"one", "two", NULL}},
    {"model.remove", {"--flag", "v", NULL}},
    {"model.remove", {"", NULL}},
    {"model.roles", {NULL}},
    {"model.roles", {"one", NULL}},
    {"model.roles", {"one", "two", NULL}},
    {"model.roles", {"--flag", "v", NULL}},
    {"model.roles", {"", NULL}},
    {"cert.issue", {NULL}},
    {"cert.issue", {"v0", NULL}},
    {"cert.issue", {"", NULL}},
    {"cert.issue", {"v0", "--days", "7", NULL}},
    {"cert.issue", {"v0", "--days", "abc", NULL}},
    {"job.cancel", {NULL}},
    {"job.cancel", {"0", NULL}},
    {"job.cancel", {"", NULL}},
    {"job.cancel", {"--job-id", "7", NULL}},
    {"job.cancel", {"0", "--reason", "x", NULL}},
    {"job.status", {NULL}},
    {"job.status", {"0", NULL}},
    {"job.status", {"", NULL}},
    {"job.status", {"--job-id", "7", NULL}},
    {"job.status", {"0", "--reason", "x", NULL}},
    {"jobs.cancel", {NULL}},
    {"jobs.cancel", {"0", NULL}},
    {"jobs.cancel", {"", NULL}},
    {"jobs.cancel", {"--job-id", "7", NULL}},
    {"jobs.cancel", {"0", "--reason", "x", NULL}},
    {"jobs.logs", {NULL}},
    {"jobs.logs", {"0", NULL}},
    {"jobs.logs", {"", NULL}},
    {"jobs.logs", {"--job-id", "7", NULL}},
    {"jobs.logs", {"0", "--reason", "x", NULL}},
    {"jobs.status", {NULL}},
    {"jobs.status", {"0", NULL}},
    {"jobs.status", {"", NULL}},
    {"jobs.status", {"--job-id", "7", NULL}},
    {"jobs.status", {"0", "--reason", "x", NULL}},
    {"kb.reembed", {NULL}},
    {"kb.reembed", {"--confirm", NULL}},
    {"kb.reembed", {"--force", NULL}},
    {"kb.reembed", {"--dry-run", NULL}},
    {"kb.reembed", {"--clear-maintenance", NULL}},
    {"kb.reembed", {"--target-dim", "7", NULL}},
    {"kb.reembed", {"--target-dim", "abc", NULL}},
    {"rules.delete", {NULL}},
    {"rules.delete", {"0", NULL}},
    {"rules.delete", {"", NULL}},
    {"rules.delete", {"--id", "7", NULL}},
    {"job.start", {NULL}},
    {"job.start", {"0", NULL}},
    {"job.start", {"", NULL}},
    {"job.start", {"--plan-id", "7", NULL}},
    {"job.start", {"0", "--parallel", "7", NULL}},
    {"job.start", {"0", "--parallel", "0", NULL}},
    {"job.start", {"0", "--parallel", "abc", NULL}},
    {"session.list", {NULL}},
    {"session.list", {"--limit", "7", NULL}},
    {"session.list", {"--limit", "0", NULL}},
    {"session.list", {"--limit", "abc", NULL}},
    {"curator.contradictions", {NULL}},
    {"curator.contradictions", {"--limit", "7", NULL}},
    {"curator.contradictions", {"--limit", "0", NULL}},
    {"curator.contradictions", {"--limit", "abc", NULL}},
    {"job.list", {NULL}},
    {"job.list", {"--limit", "7", NULL}},
    {"job.list", {"--limit", "0", NULL}},
    {"job.list", {"--limit", "abc", NULL}},
    {"jobs.list", {NULL}},
    {"jobs.list", {"--limit", "7", NULL}},
    {"jobs.list", {"--limit", "0", NULL}},
    {"jobs.list", {"--limit", "abc", NULL}},
    {"notes.search", {NULL}},
    {"notes.search", {"v0", NULL}},
    {"notes.search", {"", NULL}},
    {"notes.search", {"--query", "7", NULL}},
    {"notes.search", {"v0", "--limit", "7", NULL}},
    {"notes.search", {"v0", "--limit", "0", NULL}},
    {"notes.search", {"v0", "--limit", "abc", NULL}},
    {"cron.show", {NULL}},
    {"cron.show", {"j1", NULL}},
    {"cron.show", {"--id", "j1", NULL}},
    {"cron.show", {"", NULL}},
    {"cron.show", {"j1", "--limit", "5", NULL}},
    {"cron.show", {"j1", "--limit", "abc", NULL}},
    {"cron.run", {NULL}},
    {"cron.run", {"j1", NULL}},
    {"cron.run", {"--id", "j1", NULL}},
    {"cron.run", {"", NULL}},
    {"cron.run", {"j1", "--limit", "5", NULL}},
    {"cron.run", {"j1", "--limit", "abc", NULL}},
    {"cron.remove", {NULL}},
    {"cron.remove", {"j1", NULL}},
    {"cron.remove", {"--id", "j1", NULL}},
    {"cron.remove", {"", NULL}},
    {"cron.remove", {"j1", "--limit", "5", NULL}},
    {"cron.remove", {"j1", "--limit", "abc", NULL}},
    {"cron.history", {NULL}},
    {"cron.history", {"j1", NULL}},
    {"cron.history", {"--id", "j1", NULL}},
    {"cron.history", {"", NULL}},
    {"cron.history", {"j1", "--limit", "5", NULL}},
    {"cron.history", {"j1", "--limit", "abc", NULL}},
    {"pipeline.advance", {NULL}},
    {"pipeline.advance", {"7", NULL}},
    {"pipeline.advance", {"abc", NULL}},
    {"pipeline.advance", {"--state", "open", NULL}},
    {"pipeline.advance", {"--verdict", "pass", NULL}},
    {"pipeline.advance", {"7", "--artifact", "a", NULL}},
    {"pipeline.advance", {"7", "--remote", "r", NULL}},
    {"pipeline.cancel", {NULL}},
    {"pipeline.cancel", {"7", NULL}},
    {"pipeline.cancel", {"abc", NULL}},
    {"pipeline.cancel", {"--state", "open", NULL}},
    {"pipeline.cancel", {"--verdict", "pass", NULL}},
    {"pipeline.cancel", {"7", "--artifact", "a", NULL}},
    {"pipeline.cancel", {"7", "--remote", "r", NULL}},
    {"pipeline.gate", {NULL}},
    {"pipeline.gate", {"7", NULL}},
    {"pipeline.gate", {"abc", NULL}},
    {"pipeline.gate", {"--state", "open", NULL}},
    {"pipeline.gate", {"--verdict", "pass", NULL}},
    {"pipeline.gate", {"7", "--artifact", "a", NULL}},
    {"pipeline.gate", {"7", "--remote", "r", NULL}},
    {"pipeline.list", {NULL}},
    {"pipeline.list", {"7", NULL}},
    {"pipeline.list", {"abc", NULL}},
    {"pipeline.list", {"--state", "open", NULL}},
    {"pipeline.list", {"--verdict", "pass", NULL}},
    {"pipeline.list", {"7", "--artifact", "a", NULL}},
    {"pipeline.list", {"7", "--remote", "r", NULL}},
    {"pipeline.resume", {NULL}},
    {"pipeline.resume", {"7", NULL}},
    {"pipeline.resume", {"abc", NULL}},
    {"pipeline.resume", {"--state", "open", NULL}},
    {"pipeline.resume", {"--verdict", "pass", NULL}},
    {"pipeline.resume", {"7", "--artifact", "a", NULL}},
    {"pipeline.resume", {"7", "--remote", "r", NULL}},
    {"pipeline.show", {NULL}},
    {"pipeline.show", {"7", NULL}},
    {"pipeline.show", {"abc", NULL}},
    {"pipeline.show", {"--state", "open", NULL}},
    {"pipeline.show", {"--verdict", "pass", NULL}},
    {"pipeline.show", {"7", "--artifact", "a", NULL}},
    {"pipeline.show", {"7", "--remote", "r", NULL}},
    {"pipeline.status", {NULL}},
    {"pipeline.status", {"7", NULL}},
    {"pipeline.status", {"abc", NULL}},
    {"pipeline.status", {"--state", "open", NULL}},
    {"pipeline.status", {"--verdict", "pass", NULL}},
    {"pipeline.status", {"7", "--artifact", "a", NULL}},
    {"pipeline.status", {"7", "--remote", "r", NULL}},
};

static const char *spec_for(const char *method)
{
   for (size_t i = 0; i < sizeof(SHIPPED) / sizeof(SHIPPED[0]); i++)
      if (strcmp(SHIPPED[i].method, method) == 0)
         return SHIPPED[i].spec;
   return NULL;
}

/* Every shipped spec must have samples. Otherwise a spec added tomorrow ships
 * to every client without one line of evidence that it builds the same request
 * the marshaller does — which is the whole property this file exists for. */
static void test_every_shipped_spec_is_sampled(void)
{
   for (size_t i = 0; i < sizeof(SHIPPED) / sizeof(SHIPPED[0]); i++)
   {
      int found = 0;
      for (size_t j = 0; j < sizeof(SAMPLES) / sizeof(SAMPLES[0]) && !found; j++)
         if (strcmp(SAMPLES[j].method, SHIPPED[i].method) == 0)
            found = 1;
      if (!found)
         fail("unproven spec", SHIPPED[i].method);
   }
}

static int argv_len(const char *const *argv)
{
   int n = 0;
   while (argv[n])
      n++;
   return n;
}

/* Compare the compiled marshaller's body with the interpreter's, rendered. */
static void check_same(const sample_t *s)
{
   char *argv_buf[8];
   int argc = argv_len(s->argv);
   for (int i = 0; i < argc; i++)
      argv_buf[i] = (char *)s->argv[i];

   const char *spec_json = spec_for(s->method);
   if (!spec_json)
   {
      /* A sample for a method the server does not serve a spec for. Not
       * harmless: it reads as coverage while proving nothing. */
      fail(s->method, "sampled, but no shipped spec has this method");
      return;
   }
   cJSON *spec_doc = cJSON_Parse(spec_json);
   if (!spec_doc)
   {
      fail(s->method, "spec is not valid JSON");
      return;
   }
   if (!cli_argspec_supported(spec_doc))
   {
      fail(s->method, "spec uses something the interpreter does not know");
      cJSON_Delete(spec_doc);
      return;
   }

   cJSON *from_code = marshal_request(s->method, argc, argv_buf);
   cJSON *from_spec = cli_argspec_build(s->method, spec_doc, argc, argv_buf);

   /* Both refusing is agreement: a required argument was missing and neither
    * would have sent anything. */
   if (!from_code && !from_spec)
   {
      cJSON_Delete(spec_doc);
      return;
   }
   if (!from_code || !from_spec)
   {
      char detail[512];
      snprintf(detail, sizeof(detail), "one side refused (compiled=%s spec=%s) for argv[0]=%s",
               from_code ? "built" : "NULL", from_spec ? "built" : "NULL",
               argc > 0 ? s->argv[0] : "(none)");
      fail(s->method, detail);
   }
   else
   {
      char *a = cJSON_PrintUnformatted(from_code);
      char *b = cJSON_PrintUnformatted(from_spec);
      if (!a || !b || strcmp(a, b) != 0)
      {
         char detail[1024];
         snprintf(detail, sizeof(detail), "bodies differ\n    compiled: %s\n    spec:     %s",
                  a ? a : "(null)", b ? b : "(null)");
         fail(s->method, detail);
      }
      free(a);
      free(b);
   }
   cJSON_Delete(from_code);
   cJSON_Delete(from_spec);
   cJSON_Delete(spec_doc);
}

/* ---- spec validation ---------------------------------------------------- */

static void test_refuses_what_it_cannot_build(void)
{
   /* Each of these names something this build does not understand. Accepting
    * any of them would mean building a body from a spec written for a client
    * that knows more — the one way a served spec can be actively dangerous. */
   static const char *const bad[] = {
       "{\"fields\":[{\"json\":\"x\",\"from\":\"read_file\",\"path\":\"/etc/passwd\"}]}",
       "{\"fields\":[{\"json\":\"x\",\"from\":\"env\",\"var\":\"HOME\"}]}",
       "{\"fields\":[{\"json\":\"x\",\"from\":\"flag\",\"flag\":\"x\",\"type\":\"regex\"}]}",
       "{\"fields\":[{\"json\":\"x\",\"from\":\"flag\"}]}",       /* no flag named   */
       "{\"fields\":[{\"json\":\"x\",\"from\":\"positional\"}]}", /* no index        */
       "{\"fields\":[{\"from\":\"flag\",\"flag\":\"x\"}]}",       /* no json name    */
       "{\"fields\":[{\"json\":\"\",\"from\":\"flag\",\"flag\":\"x\"}]}",
       "{\"fields\":{\"json\":\"x\"}}", /* fields not array*/
       "{\"bool_flags\":\"json\"}",     /* bools not array */
       "{\"fields\":[{\"json\":\"x\",\"from\":\"positional\",\"index\":-1}]}",
       "{\"fields\":[{\"json\":\"x\",\"from\":\"positional\",\"index\":9999}]}",
       "[]",
       "\"none\"",
       NULL,
   };
   for (int i = 0; bad[i]; i++)
   {
      cJSON *doc = cJSON_Parse(bad[i]);
      if (!doc)
      {
         fail("spec-validation", bad[i]);
         continue;
      }
      if (cli_argspec_supported(doc))
         fail("spec-validation accepted an unbuildable spec", bad[i]);
      /* And the build path must agree with the validator, not just the
       * validator alone: a caller that skipped the check must still get NULL. */
      char *argv[] = {(char *)"x"};
      cJSON *req = cli_argspec_build("some.method", doc, 1, argv);
      if (req)
      {
         fail("build accepted an unbuildable spec", bad[i]);
         cJSON_Delete(req);
      }
      cJSON_Delete(doc);
   }

   /* NULL and a non-object are refused without crashing. */
   if (cli_argspec_supported(NULL))
      fail("spec-validation", "accepted NULL");
   if (cli_argspec_build("m", NULL, 0, NULL))
      fail("build", "accepted NULL spec");

   /* An empty spec is legal — it is the no-argument case written out — and
    * builds the bare method envelope. */
   cJSON *empty = cJSON_Parse("{}");
   cJSON *req = cli_argspec_build("some.method", empty, 0, NULL);
   if (!req)
      fail("build", "refused an empty spec, which is the no-argument case");
   else
   {
      const cJSON *m = cJSON_GetObjectItemCaseSensitive(req, "method");
      if (!cJSON_IsString(m) || strcmp(m->valuestring, "some.method") != 0)
         fail("build", "empty spec did not carry the method");
      cJSON_Delete(req);
   }
   cJSON_Delete(empty);
}

static void test_number_is_refused_not_coerced(void)
{
   /* A field the spec calls a number must be refused when it is not one. The
    * compiled marshallers already refuse rather than round for exactly this
    * reason (see kb.grant's team_id), and a served spec must not be the softer
    * path into the same request. */
   cJSON *spec = cJSON_Parse("{\"usage\":\"u\",\"fields\":[{\"json\":\"n\",\"from\":\"flag\","
                             "\"flag\":\"n\",\"type\":\"number\",\"required\":true}]}");
   char *bad[] = {(char *)"--n", (char *)"12x"};
   cJSON *req = cli_argspec_build("m", spec, 2, bad);
   if (req)
   {
      fail("number", "coerced '12x' instead of refusing it");
      cJSON_Delete(req);
   }
   char *good[] = {(char *)"--n", (char *)"12"};
   req = cli_argspec_build("m", spec, 2, good);
   if (!req)
      fail("number", "refused a valid number");
   else
   {
      const cJSON *n = cJSON_GetObjectItemCaseSensitive(req, "n");
      if (!cJSON_IsNumber(n) || n->valuedouble != 12)
         fail("number", "did not emit 12 as a JSON number");
      cJSON_Delete(req);
   }
   cJSON_Delete(spec);
}

int main(void)
{
   test_every_shipped_spec_is_sampled();
   for (size_t i = 0; i < sizeof(SAMPLES) / sizeof(SAMPLES[0]); i++)
      check_same(&SAMPLES[i]);
   test_refuses_what_it_cannot_build();
   test_number_is_refused_not_coerced();

   if (g_fail == 0)
      printf("PASS test_cli_argspec (%zu shipped specs, %zu differential samples)\n",
             sizeof(SHIPPED) / sizeof(SHIPPED[0]), sizeof(SAMPLES) / sizeof(SAMPLES[0]));
   else
      printf("FAIL test_cli_argspec: %d check(s) failed\n", g_fail);
   return g_fail == 0 ? 0 : 1;
}
