/* memory_embed_texts: N texts must cost ONE embedder round trip, and any
 * malformed batch response must fall back rather than write partial vectors.
 *
 * The per-file shape this replaced made a corpus ingest take hours: the embedder
 * serves ~2000 vectors/min batched and ~800 one at a time, so "one request per
 * text" was the difference between a usable ingest and an unusable one. The
 * request COUNT is therefore the behaviour under test, not an implementation
 * detail. */
#include "support/mock_agent_http.h"
#include "embed_input_type.h"
#include "aimee.h"  /* KIND_COUNT, required by memory.h */
#include "memory.h" /* MEMORY_EMBED_TEST_FIXTURE */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int memory_embed_texts(const char *const *texts, int n, const char *command,
                       embed_input_type_t input_type, float *out, int dim);

#define DIM   3
#define NTEXT 4

static int s_posts;
static int s_last_timeout_ms;
static char s_last_url[512];
static char s_last_body[4096];

/* Returns NTEXT rows of DIM floats, each row r as [r, r+0.5, r+0.25]. */
static int post_batch_ok(const char *url, const char *auth_header, const char *body,
                         char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)auth_header;
   (void)extra_headers;
   s_last_timeout_ms = timeout_ms;
   s_posts++;
   snprintf(s_last_url, sizeof(s_last_url), "%s", url ? url : "");
   snprintf(s_last_body, sizeof(s_last_body), "%s", body ? body : "");
   char *out = malloc(1024);
   char *p = out;
   p += sprintf(p, "[");
   for (int r = 0; r < NTEXT; r++)
      p += sprintf(p, "%s[%d.0,%d.5,%d.25]", r ? "," : "", r, r, r);
   sprintf(p, "]");
   *response_buf = out;
   return 200;
}

/* One row short: a count mismatch must discard the whole batch. */
static int post_batch_short(const char *url, const char *auth_header, const char *body,
                            char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)body;
   (void)timeout_ms;
   (void)extra_headers;
   s_posts++;
   *response_buf = strdup("[[1.0,2.0,3.0]]");
   return 200;
}

/* Right row count, wrong width: a short vector is a different point in the
 * vector space, not a usable approximation of one. */
static int post_batch_narrow(const char *url, const char *auth_header, const char *body,
                             char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)body;
   (void)timeout_ms;
   (void)extra_headers;
   s_posts++;
   *response_buf = strdup("[[1.0,2.0],[1.0,2.0],[1.0,2.0],[1.0,2.0]]");
   return 200;
}

static int post_fail(const char *url, const char *auth_header, const char *body,
                     char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)body;
   (void)response_buf;
   (void)timeout_ms;
   (void)extra_headers;
   s_posts++;
   return 500;
}

/* ---- embed round-trip bound --------------------------------------------------
 *
 * RED-GREEN. This was hardcoded at 30 s. That is ~20x an UNLOADED 128-text batch
 * (1.5 s measured against bekko-a25m) but not 20x a loaded one: the kb embeds
 * from up to KB_WORKER_MAX threads against a ThreadingHTTPServer, each request
 * running torch with EMBEDDER_THREADS threads on a 4-CPU container. Every
 * in-flight batch slows together and the first past 30 s makes the kb drop the
 * connection -- surfacing as BrokenPipeError in the embedder and
 * "knowledge service /v1/code/build did not respond" from `kb build`, which is
 * what killed the first full-capability cell.
 *
 * A bound below the real cost turns a slow build into a FAILED one. Assert the
 * headroom, and that the transport is actually handed the tunable value rather
 * than a constant that drifts away from it. */
static void test_embed_timeout_clears_a_loaded_batch(void)
{
   unsetenv("AIMEE_EMBED_HTTP_TIMEOUT_MS");
   /* 30 s is what broke it; demand real headroom over a loaded 128-batch. */
   assert(memory_embed_http_timeout_ms() >= 60000);
}

static void test_embed_timeout_is_operator_tunable(void)
{
   setenv("AIMEE_EMBED_HTTP_TIMEOUT_MS", "45000", 1);
   assert(memory_embed_http_timeout_ms() == 45000);
   /* Garbage and out-of-range fall back rather than disabling the bound. */
   setenv("AIMEE_EMBED_HTTP_TIMEOUT_MS", "0", 1);
   assert(memory_embed_http_timeout_ms() >= 60000);
   setenv("AIMEE_EMBED_HTTP_TIMEOUT_MS", "banana", 1);
   assert(memory_embed_http_timeout_ms() >= 60000);
   unsetenv("AIMEE_EMBED_HTTP_TIMEOUT_MS");
}

/* The bound is only real if the transport receives it. A constant left behind at
 * a call site is exactly how the 30 s survived unnoticed. */
static void test_transport_receives_the_configured_bound(void)
{
   setenv("AIMEE_EMBED_HTTP_TIMEOUT_MS", "77000", 1);
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(post_batch_ok);
   s_posts = 0;
   s_last_timeout_ms = -1;
   const char *texts[NTEXT] = {"a", "b", "c", "d"};
   float out[NTEXT * DIM];
   assert(memory_embed_texts(texts, NTEXT, "http://embedder:8760", EMBED_INPUT_DOCUMENT, out,
                             DIM) == NTEXT);
   assert(s_posts == 1);
   assert(s_last_timeout_ms == 77000);
   unsetenv("AIMEE_EMBED_HTTP_TIMEOUT_MS");
}

int main(void)
{
   const char *texts[NTEXT] = {"alpha", "beta", "gamma", "delta"};
   float out[NTEXT * DIM];

   /* One round trip for the whole batch, at the batch endpoint, carrying polarity. */
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(post_batch_ok);
   s_posts = 0;
   memset(out, 0, sizeof(out));
   assert(memory_embed_texts(texts, NTEXT, "http://embedder:8760", EMBED_INPUT_DOCUMENT, out,
                             DIM) == NTEXT);
   assert(s_posts == 1);
   assert(strstr(s_last_url, "/embed_batch") != NULL);
   assert(strstr(s_last_url, "input_type=document") != NULL);
   /* Every text reaches the embedder, in order, as one JSON array. */
   assert(strcmp(s_last_body, "[\"alpha\",\"beta\",\"gamma\",\"delta\"]") == 0);
   /* Row-major: text i occupies out[i*DIM .. i*DIM+DIM-1]. */
   for (int r = 0; r < NTEXT; r++)
   {
      assert(out[r * DIM + 0] == (float)r);
      assert(out[r * DIM + 1] == (float)r + 0.5f);
      assert(out[r * DIM + 2] == (float)r + 0.25f);
   }

   /* Polarity is not hardcoded: a query batch must ask for the query prefix. */
   mock_agent_http_set_post_handler(post_batch_ok);
   s_posts = 0;
   assert(memory_embed_texts(texts, NTEXT, "http://embedder:8760", EMBED_INPUT_QUERY, out, DIM) ==
          NTEXT);
   assert(strstr(s_last_url, "input_type=query") != NULL);

   /* Too few rows: report failure and leave the caller to embed per text. */
   mock_agent_http_set_post_handler(post_batch_short);
   s_posts = 0;
   memset(out, 7, sizeof(out));
   assert(memory_embed_texts(texts, NTEXT, "http://embedder:8760", EMBED_INPUT_DOCUMENT, out,
                             DIM) == 0);
   assert(s_posts == 1);

   /* Right count, wrong width: still a whole-batch reject, and nothing written. */
   mock_agent_http_set_post_handler(post_batch_narrow);
   s_posts = 0;
   float sentinel[NTEXT * DIM];
   memset(sentinel, 0x5a, sizeof(sentinel));
   memcpy(out, sentinel, sizeof(out));
   assert(memory_embed_texts(texts, NTEXT, "http://embedder:8760", EMBED_INPUT_DOCUMENT, out,
                             DIM) == 0);
   assert(memcmp(out, sentinel, sizeof(out)) == 0);

   /* Transport failure: fall back, do not fabricate vectors. */
   mock_agent_http_set_post_handler(post_fail);
   s_posts = 0;
   assert(memory_embed_texts(texts, NTEXT, "http://embedder:8760", EMBED_INPUT_DOCUMENT, out,
                             DIM) == 0);
   assert(s_posts == 1);

   /* The builtin is in-process; batching it would only add a JSON encode, so the
    * caller is sent to the per-text path that computes the same vectors. */
   mock_agent_http_set_post_handler(post_batch_ok);
   s_posts = 0;
   assert(memory_embed_texts(texts, NTEXT, MEMORY_EMBED_TEST_FIXTURE, EMBED_INPUT_DOCUMENT, out,
                             DIM) == 0);
   assert(s_posts == 0);

   /* Degenerate inputs never reach the embedder. */
   s_posts = 0;
   assert(memory_embed_texts(texts, 0, "http://embedder:8760", EMBED_INPUT_DOCUMENT, out, DIM) ==
          0);
   const char *with_empty[2] = {"alpha", ""};
   assert(memory_embed_texts(with_empty, 2, "http://embedder:8760", EMBED_INPUT_DOCUMENT, out,
                             DIM) == 0);
   assert(s_posts == 0);

   test_embed_timeout_clears_a_loaded_batch();
   test_embed_timeout_is_operator_tunable();
   test_transport_receives_the_configured_bound();
   printf("test_memory_embed_batch: OK\n");
   return 0;
}
