/* test_kb_graph.c: unit tests for the code-graph P1 build orchestrator and the
 * derived edge-provenance tag. DB2 is stubbed so the idempotency control flow
 * (skip-when-unchanged vs publish-a-new-generation) is driven deterministically
 * without a live Postgres. */
#include "kb_service_graph.h"
#include "modules/db2/c/code_projection.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- controllable DB2 stubs ---- */
static char g_fp[64] = "";      /* fingerprint the project "currently" hashes to */
static char g_visible[64] = ""; /* fingerprint stored on the visible generation  */
static int g_synced = 0;        /* db2_code_projection_sync_project call count    */
static int g_published = 0;     /* db2_code_projection_generation_publish count   */
static int g_aborted = 0;

int db2_is_initialized(void)
{
   return 1;
}
int db2_code_projection_project_fingerprint(const char *project, char *out, size_t out_len)
{
   (void)project;
   snprintf(out, out_len, "%s", g_fp);
   return g_fp[0] ? 0 : -1; /* empty g_fp simulates a fingerprint error */
}
int db2_code_projection_visible_source_hash(const char *project, char *out, size_t out_len)
{
   (void)project;
   snprintf(out, out_len, "%s", g_visible);
   return 0;
}
int64_t db2_code_projection_generation_create(const char *project)
{
   (void)project;
   return 42;
}
int db2_code_projection_generation_set_source_hash(int64_t gen, const char *h)
{
   (void)gen;
   (void)h;
   return 0;
}
int64_t db2_code_projection_sync_project(const char *project, int64_t gen)
{
   (void)project;
   (void)gen;
   g_synced++;
   return 7; /* pretend 7 edges */
}
int db2_code_projection_generation_publish(int64_t gen, const char *project)
{
   (void)gen;
   (void)project;
   g_published++;
   return 0;
}
int db2_code_projection_generation_abort(int64_t gen, const char *err)
{
   (void)gen;
   (void)err;
   g_aborted++;
   return 0;
}

/* Controllable edge fixture + captured community persistence, for the
 * kb_graph_persist_communities path that runs after a publish. */
static const code_projection_edge_t *g_edges = NULL;
static int g_n_edges = 0;
static int64_t g_persist_gen = 0;
static int g_persist_n = -1;
static code_projection_community_t g_persist_rows[64];

int db2_code_projection_list_edges_for_gen(int64_t gen, code_projection_edge_t *out, int max)
{
   (void)gen;
   int n = g_n_edges < max ? g_n_edges : max;
   for (int i = 0; i < n; i++)
      out[i] = g_edges[i];
   return n;
}
int db2_code_projection_communities_replace(int64_t gen, const char *project,
                                            const code_projection_community_t *rows, int n)
{
   (void)project;
   g_persist_gen = gen;
   g_persist_n = n;
   for (int i = 0; i < n && i < (int)(sizeof(g_persist_rows) / sizeof(g_persist_rows[0])); i++)
      g_persist_rows[i] = rows[i];
   return 0;
}
static const char *persisted_community_of(const char *node)
{
   for (int i = 0; i < g_persist_n; i++)
      if (strcmp(g_persist_rows[i].node_id, node) == 0)
         return g_persist_rows[i].community_id;
   return NULL;
}

/* ---- link-only stubs for the rest of kb_service_graph.o (unused here) ---- */
struct cJSON;
struct cJSON *jo_ok(void)
{
   return 0;
}
int db2_entity_edge_explain_by_entity(const char *e, void *out, int n)
{
   (void)e;
   (void)out;
   (void)n;
   return 0;
}
int db2_entity_node_get(const char *e, void *out)
{
   (void)e;
   (void)out;
   return -1;
}
struct cJSON *db2_kb_service_code_audit_json(const char *p, int n)
{
   (void)p;
   (void)n;
   return 0;
}
int kb_send_error(int fd, const char *m)
{
   (void)fd;
   (void)m;
   return 0;
}
int kb_send_response(int fd, struct cJSON *r)
{
   (void)fd;
   (void)r;
   return 0;
}

static void test_provenance(void)
{
   /* deterministic AST/index edge */
   assert(strcmp(kb_graph_edge_provenance("code_projection", 0), "structural") == 0);
   /* any positive structural-trust weight is structural regardless of origin */
   assert(strcmp(kb_graph_edge_provenance("session", 3), "structural") == 0);
   /* raw session co-occurrence, no structural grounding -> ambiguous */
   assert(strcmp(kb_graph_edge_provenance("session", 0), "ambiguous") == 0);
   /* curator/semantic-derived -> inferred */
   assert(strcmp(kb_graph_edge_provenance("curator", 0), "inferred") == 0);
   assert(strcmp(kb_graph_edge_provenance(NULL, 0), "inferred") == 0);
   printf("  test_provenance: ok\n");
}

static void test_idempotent_build(void)
{
   int rebuilt = -1;
   int64_t r;

   /* (1) Unchanged: fingerprint == the visible generation's hash -> SKIP, no sync. */
   snprintf(g_fp, sizeof(g_fp), "abc");
   snprintf(g_visible, sizeof(g_visible), "abc");
   g_synced = g_published = 0;
   r = kb_graph_build_project_if_changed("p", &rebuilt);
   assert(r == 0 && rebuilt == 0 && g_synced == 0 && g_published == 0);

   /* (2) Changed: fingerprint differs -> rebuild (sync + publish), edges returned. */
   snprintf(g_visible, sizeof(g_visible), "def");
   g_synced = g_published = 0;
   r = kb_graph_build_project_if_changed("p", &rebuilt);
   assert(r == 7 && rebuilt == 1 && g_synced == 1 && g_published == 1);

   /* (3) First build: no visible generation yet -> rebuild. */
   g_visible[0] = '\0';
   g_synced = g_published = 0;
   r = kb_graph_build_project_if_changed("p", &rebuilt);
   assert(r == 7 && rebuilt == 1 && g_synced == 1 && g_published == 1);

   /* (4) Fingerprint error -> -1, no work. */
   g_fp[0] = '\0';
   g_synced = g_published = 0;
   r = kb_graph_build_project_if_changed("p", &rebuilt);
   assert(r == -1 && rebuilt == 0 && g_synced == 0 && g_published == 0);

   printf("  test_idempotent_build: ok\n");
}

/* After a publish, the derived community membership is computed from the
 * generation's edges and persisted (keyed by generation id). Drives the real
 * build path with a two-triangle fixture through the DB2 stubs. */
static void test_community_persist(void)
{
   code_projection_edge_t edges[] = {
       {"a", "calls", "b", 5}, {"b", "calls", "c", 5}, {"a", "calls", "c", 5},
       {"x", "calls", "y", 5}, {"y", "calls", "z", 5}, {"x", "calls", "z", 5},
       {"c", "calls", "x", 1},
   };
   g_edges = edges;
   g_n_edges = 7;
   g_persist_gen = 0;
   g_persist_n = -1;

   snprintf(g_fp, sizeof(g_fp), "aaa");
   g_visible[0] = '\0'; /* first build -> rebuild + publish */
   int rebuilt = -1;
   int64_t r = kb_graph_build_project_if_changed("p", &rebuilt);
   assert(r == 7 && rebuilt == 1);

   assert(g_persist_gen == 42); /* keyed by the generation id */
   assert(g_persist_n == 6);    /* six distinct nodes */
   assert(strcmp(persisted_community_of("b"), "a") == 0);
   assert(strcmp(persisted_community_of("c"), "a") == 0);
   assert(strcmp(persisted_community_of("z"), "x") == 0);
   assert(strcmp(persisted_community_of("a"), persisted_community_of("x")) != 0);

   g_edges = NULL;
   g_n_edges = 0;
   printf("  test_community_persist: ok\n");
}

int main(void)
{
   printf("test_kb_graph:\n");
   test_provenance();
   test_idempotent_build();
   test_community_persist();
   printf("ALL PASS\n");
   return 0;
}
