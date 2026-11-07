/* Owned REAL PostgreSQL qualification for the generic SQL source lifecycle.
 * Observer callbacks record fixture events; native publisher/provider admission
 * is qualified separately by the private module integration harness. */
#include "index_observer_internal.h"
#include "kb_store.h"
#include "kb_store_internal.h"
#include "code_index.h"
#include "canonical_index.h"
#include "code_project_lifecycle.h"
#include "db_postgres.h"
#include "config.h"
#include "log.h"
#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#define CHECK(x)                                                                                   \
   do                                                                                              \
   {                                                                                               \
      if (!(x))                                                                                    \
      {                                                                                            \
         fprintf(stderr, "check failed line %d: %s\n", __LINE__, #x);                              \
         exit(1);                                                                                  \
      }                                                                                            \
   } while (0)
static const char *connection_info;
static atomic_int available = 1, fail_file, at_ready, release_ready, writer_b_entered,
                  creator_started, creator_entered, pause_begin, at_begin, release_begin;
static _Thread_local int writer;
static const char *denied_project;
struct publication
{
   char *project, *body;
   size_t files;
};
static void sql(void *c, const char *s)
{
   char e[512] = "";
   if (aimee_pg_exec(c, s, e, sizeof(e)))
   {
      fprintf(stderr, "SQL failed: %s\n", e);
      exit(1);
   }
}
static void *connect_owned(void)
{
   char error[256] = "";
   void *c = aimee_pg_open(connection_info, error, sizeof(error));
   CHECK(c);
   return c;
}
static long scalar(void *c, const char *s)
{
   char e[256] = "";
   aimee_pg_stmt_t *q = aimee_pg_prepare(c, s, e, sizeof(e));
   CHECK(q);
   CHECK(aimee_pg_step(q, e, sizeof(e)) == AIMEE_PG_ROW);
   long n = (long)aimee_pg_column_int64(q, 0);
   aimee_pg_finalize(q);
   return n;
}
static void event(const char *project, const char *state, const char *body)
{
   void *c = connect_owned();
   char e[256] = "";
   aimee_pg_stmt_t *q = aimee_pg_prepare(
       c, "INSERT INTO observer_events(project,state,body) VALUES(?1,?2,?3)", e, sizeof(e));
   CHECK(q);
   aimee_pg_bind_text(q, "?1", project);
   aimee_pg_bind_text(q, "?2", state);
   aimee_pg_bind_text(q, "?3", body ? body : "");
   CHECK(aimee_pg_step(q, e, sizeof(e)) == AIMEE_PG_DONE);
   aimee_pg_finalize(q);
   aimee_pg_close(c);
}
static int probe(void *context)
{
   (void)context;
   return atomic_load(&available) ? 0 : -1;
}
static int begin(void *context, const char *project, int64_t generation, const char *operation,
                 void **out)
{
   (void)context;
   (void)operation;
   CHECK(generation > 0);
   if (denied_project && strcmp(project, denied_project) == 0)
      return -1;
   struct publication *p = calloc(1, sizeof(*p));
   CHECK(p);
   p->project = strdup(project);
   CHECK(p->project);
   *out = p;
   event(project, "building", "");
   if (atomic_load(&pause_begin))
   {
      atomic_store(&at_begin, 1);
      while (!atomic_load(&release_begin))
      {
         struct timespec t = {0, 1000000};
         nanosleep(&t, NULL);
      }
   }

   if (writer == 2)
      atomic_store(&writer_b_entered, 1);
   if (writer == 3)
      atomic_store(&creator_entered, 1);
   return 0;
}
static int file(void *context, void *handle, const char *path, const char *text, size_t length)
{
   (void)context;
   struct publication *p = handle;
   CHECK(length == strlen(text));
   CHECK(path && path[0]);
   if (atomic_exchange(&fail_file, 0))
      return -1;
   p->files++;
   if (strcmp(path, "main.txt") == 0)
   {
      free(p->body);
      p->body = strdup(text);
      CHECK(p->body);
   }
   return 0;
}
static int ready(void *context, void *handle, size_t count)
{
   (void)context;
   struct publication *p = handle;
   CHECK(count == p->files);
   event(p->project, "ready", p->body);
   if (writer == 1)
   {
      atomic_store(&at_ready, 1);
      while (!atomic_load(&release_ready))
      {
         struct timespec t = {0, 1000000};
         nanosleep(&t, NULL);
      }
   }
   return 0;
}
static int unavailable(void *context, void *handle, const char *state)
{
   (void)context;
   event(((struct publication *)handle)->project, state, "");
   return 0;
}
static void release(void *context, void *handle)
{
   (void)context;
   struct publication *p = handle;
   free(p->project);
   free(p->body);
   free(p);
}
static aimee_kb_index_observer_t callbacks = {1, probe, begin, file, ready, unavailable, release};
/* This fixture intentionally selects the existing configuration with optional
 * CSS/git projections disabled. No source SQL, locking or audit is stubbed. */
int config_kb_purge_fence_ttl_s(void)
{
   return 0;
}
int config_present(void)
{
   return 0;
}
int config_css_style_graph_enabled(void)
{
   return 0;
}
int config_code_cochange_git_enabled(void)
{
   return 0;
}
int config_audit_worm_enabled(void)
{
   return 1;
}
const char *config_default_dir(void)
{
   return "/nonexistent-owned-observer-test";
}
static int64_t file_id;
static void replace(const char *text)
{
   code_index_file_data_t data = {0};
   data.content = text;
   CHECK(kb_store_code_index_file_replace(file_id, &data) == 0);
}
static void *run_writer(void *arg)
{
   writer = (int)(intptr_t)arg;
   kb_store_lease_begin();
   {
      KB_INDEX_MUTATION(g);
      CHECK(kb_index_mutation_begin(&g, kb_store_conn(), "p", "two_writer", 0) == 0);
      replace(writer == 1 ? "writer A" : "writer B");
      CHECK(kb_index_mutation_finish(&g) == 0);
   }
   kb_store_lease_end();
   return NULL;
}
static void *create_during_startup(void *arg)
{
   (void)arg;
   writer = 3;
   kb_store_lease_begin();
   atomic_store(&creator_started, 1);
   CHECK(kb_store_code_index_project_upsert("new_during_startup", "remote") > 0);
   kb_store_lease_end();
   return NULL;
}
static long scalar_text(void *c, const char *sql_text, const char *text)
{
   char e[256] = "";
   aimee_pg_stmt_t *q = aimee_pg_prepare(c, sql_text, e, sizeof(e));
   CHECK(q);
   aimee_pg_bind_text(q, "?1", text);
   CHECK(aimee_pg_step(q, e, sizeof(e)) == AIMEE_PG_ROW);
   long result = (long)aimee_pg_column_int64(q, 0);
   aimee_pg_finalize(q);
   return result;
}
struct seal_call
{
   const char *scan;
   int result;
};
static void *seal_in_thread(void *value)
{
   struct seal_call *call = value;
   kb_store_lease_begin();
   canonical_index_seal_result_t out;
   call->result = canonical_index_scan_seal(call->scan, 1, &out);
   kb_store_lease_end();
   return NULL;
}
static void project_boundary_tests(void *c)
{
   char long_project[301], prefix[256], maximum[1025], oversized[1026];
   memset(long_project, 'L', 300);
   long_project[300] = 0;
   memcpy(prefix, long_project, 255);
   prefix[255] = 0;
   memset(maximum, 'M', 1024);
   maximum[1024] = 0;
   memset(oversized, 'N', 1025);
   oversized[1025] = 0;
   CHECK(canonical_index_scan_begin(prefix, "remote", "prefix-255", NULL) == 0);
   CHECK(canonical_index_scan_begin(long_project, "remote", "long-300", NULL) == 0);
   canonical_index_file_input_t input = {.rel_path = "long.txt",
                                         .content = "original long project"};
   int count = 0;
   CHECK(canonical_index_scan_stage("long-300", &input, 1, &count) == 0 && count == 1);
   long prefix_events =
       scalar_text(c, "SELECT count(*) FROM observer_events WHERE project=?1", prefix);
   denied_project = long_project;
   canonical_index_seal_result_t out;
   CHECK(canonical_index_scan_seal("long-300", 1, &out) < 0);
   CHECK(scalar_text(
             c, "SELECT count(*) FROM files f JOIN projects p ON p.id=f.project_id WHERE p.name=?1",
             long_project) == 0);
   CHECK(scalar_text(c, "SELECT count(*) FROM observer_events WHERE project=?1", prefix) ==
         prefix_events);
   denied_project = NULL;
   CHECK(canonical_index_scan_seal("long-300", 1, &out) == 0);
   // ID-based mutations must resolve the exact same full logical project, not
   // its 255-byte prefix or another project with a matching leading name.
   int64_t id = scalar_text(c, "SELECT id FROM projects WHERE name=?1", long_project);
   int64_t source_file = scalar_text(
       c, "SELECT f.id FROM files f JOIN projects p ON p.id=f.project_id WHERE p.name=?1",
       long_project);
   denied_project = long_project;
   code_index_file_data_t data = {0};
   data.content = "must not replace long project";
   CHECK(kb_store_code_index_file_replace(source_file, &data) < 0);
   CHECK(kb_store_code_index_purge_files_matching(id, "%") < 0);
   CHECK(
       scalar(c,
              "SELECT count(*) FROM file_contents WHERE content='must not replace long project'") ==
       0);
   denied_project = NULL;
   // A concurrent staging-identity change after lookup/guard acquisition must
   // not transplant a seal onto a different SQL project ID under the old guard.
   CHECK(canonical_index_scan_begin(long_project, "remote", "alias-scan", NULL) == 0);
   input.rel_path = "aliased.txt";
   input.content = "must not enter prefix project";
   CHECK(canonical_index_scan_stage("alias-scan", &input, 1, &count) == 0);
   atomic_store(&pause_begin, 1);
   struct seal_call call = {"alias-scan", 0};
   pthread_t thread;
   CHECK(pthread_create(&thread, NULL, seal_in_thread, &call) == 0);
   while (!atomic_load(&at_begin))
   {
      struct timespec t = {0, 1000000};
      nanosleep(&t, NULL);
   }
   char error[256] = "";
   aimee_pg_stmt_t *q = aimee_pg_prepare(
       c,
       "UPDATE code_scan_sessions SET project_id=(SELECT id FROM projects WHERE "
       "name=?1),baseline_revision=(SELECT ps.revision FROM code_index_project_state ps JOIN "
       "projects p ON p.id=ps.project_id WHERE p.name=?1) WHERE scan_id='alias-scan'",
       error, sizeof(error));
   CHECK(q);
   aimee_pg_bind_text(q, "?1", prefix);
   CHECK(aimee_pg_step(q, error, sizeof(error)) == AIMEE_PG_DONE);
   aimee_pg_finalize(q);
   atomic_store(&release_begin, 1);
   CHECK(pthread_join(thread, NULL) == 0);
   atomic_store(&pause_begin, 0);
   CHECK(call.result < 0);
   CHECK(
       scalar(c,
              "SELECT count(*) FROM file_contents WHERE content='must not enter prefix project'") ==
       0);
   CHECK(canonical_index_scan_begin(maximum, "remote", "maximum-1024", NULL) == 0);
   input.rel_path = "maximum.txt";
   input.content = "full maximum identity";
   CHECK(canonical_index_scan_stage("maximum-1024", &input, 1, &count) == 0);
   CHECK(canonical_index_scan_seal("maximum-1024", 1, &out) == 0);
   CHECK(scalar_text(
             c, "SELECT count(*) FROM files f JOIN projects p ON p.id=f.project_id WHERE p.name=?1",
             maximum) == 1);
   CHECK(canonical_index_scan_begin(oversized, "remote", "oversized-1025", NULL) < 0);
   CHECK(scalar_text(c, "SELECT count(*) FROM projects WHERE name=?1", oversized) == 0);
   // Legacy oversized persisted identities must fail BEFORE any callback too.
   q = aimee_pg_prepare(
       c,
       "INSERT INTO projects(name,root,scanned_at,current_generation,lifecycle_state) "
       "VALUES(?1,'remote','t',1,'current') RETURNING id",
       error, sizeof(error));
   CHECK(q);
   aimee_pg_bind_text(q, "?1", oversized);
   CHECK(aimee_pg_step(q, error, sizeof(error)) == AIMEE_PG_ROW);
   int64_t badid = aimee_pg_column_int64(q, 0);
   aimee_pg_finalize(q);
   q = aimee_pg_prepare(
       c, "INSERT INTO code_index_project_state(project_id,revision,sealed_at) VALUES(?1,0,'')",
       error, sizeof(error));
   CHECK(q);
   aimee_pg_bind_int64(q, "?1", badid);
   CHECK(aimee_pg_step(q, error, sizeof(error)) == AIMEE_PG_DONE);
   aimee_pg_finalize(q);
   q = aimee_pg_prepare(c,
                        "INSERT INTO "
                        "code_scan_sessions(scan_id,project_id,generation,baseline_revision,state,"
                        "created_at,sealed_at) VALUES('legacy-oversized',?1,1,0,'open','t','')",
                        error, sizeof(error));
   CHECK(q);
   aimee_pg_bind_int64(q, "?1", badid);
   CHECK(aimee_pg_step(q, error, sizeof(error)) == AIMEE_PG_DONE);
   aimee_pg_finalize(q);
   long events = scalar(c, "SELECT count(*) FROM observer_events");
   CHECK(canonical_index_scan_seal("legacy-oversized", 0, &out) < 0);
   CHECK(scalar(c, "SELECT count(*) FROM observer_events") == events);
   // This deliberately injected unsupported legacy row must not contaminate
   // unrelated later all-project startup checks in this qualification fixture.
   q = aimee_pg_prepare(c, "DELETE FROM projects WHERE id=?1", error, sizeof(error));
   CHECK(q);
   aimee_pg_bind_int64(q, "?1", badid);
   CHECK(aimee_pg_step(q, error, sizeof(error)) == AIMEE_PG_DONE);
   aimee_pg_finalize(q);
}
int main(void)
{
   connection_info = getenv("AIMEE_INDEX_OBSERVER_TEST_DSN");
   CHECK(connection_info && connection_info[0]);
   kb_store_set_embedding_dim(4);
   kb_store_set_schema_readonly(1);
   CHECK(kb_store_init(connection_info) == 0);
   void *c = kb_store_conn();
   CHECK(c);

   aimee_kb_index_observer_require();
   CHECK(aimee_kb_index_observer_required());
   CHECK(kb_store_code_index_project_upsert("absent_callback", "remote") < 0);
   CHECK(scalar(c, "SELECT count(*) FROM projects WHERE name='absent_callback'") == 0);
   CHECK(aimee_kb_index_observer_register(&callbacks, NULL) == 0);
   atomic_store(&available, 0);
   CHECK(kb_store_code_index_project_upsert("unavailable_callback", "remote") < 0);
   CHECK(scalar(c, "SELECT count(*) FROM projects WHERE name='unavailable_callback'") == 0);
   atomic_store(&available, 1);
   {
      KB_INDEX_MUTATION(empty_startup);
      CHECK(kb_index_observer_startup_begin(&empty_startup, c) == 0);
      CHECK(aimee_kb_index_observer_register(NULL, NULL) < 0);
      CHECK(kb_index_mutation_finish(&empty_startup) == 0);
   }

   {
      KB_INDEX_MUTATION(g);
      CHECK(kb_index_mutation_begin(&g, c, "p", "nested_source", 1) == 0);
      CHECK(aimee_kb_index_observer_register(NULL, NULL) < 0);
      int64_t project = kb_store_code_index_project_upsert("p", "remote");
      CHECK(project > 0);
      file_id = kb_store_code_index_file_upsert(project, "main.txt", "2026-10-25T00:00:00Z");
      CHECK(file_id > 0);
      replace("initial");
      CHECK(kb_index_mutation_finish(&g) == 0);
   }
   CHECK(scalar(c, "SELECT count(*) FROM observer_events WHERE project='p' AND state='building'") ==
         1);
   CHECK(scalar(c, "SELECT count(*) FROM observer_events WHERE project='p' AND state='ready'") ==
         1);
   long reads = scalar(c, "SELECT count(*) FROM observer_events");
   int64_t generation;
   CHECK(kb_store_code_index_project_current_generation("p", &generation) == 0);
   CHECK(generation == 1);
   CHECK(scalar(c, "SELECT count(*) FROM observer_events") == reads);
   pthread_t a, b;
   CHECK(pthread_create(&a, NULL, run_writer, (void *)(intptr_t)1) == 0);
   while (!atomic_load(&at_ready))
   {
      struct timespec t = {0, 1000000};
      nanosleep(&t, NULL);
   }
   CHECK(pthread_create(&b, NULL, run_writer, (void *)(intptr_t)2) == 0);
   struct timespec pause = {0, 200000000};
   nanosleep(&pause, NULL);
   CHECK(!atomic_load(&writer_b_entered));
   CHECK(scalar(c, "SELECT count(*) FROM file_contents WHERE content='writer A'") == 1);
   atomic_store(&release_ready, 1);
   CHECK(pthread_join(a, NULL) == 0);
   CHECK(pthread_join(b, NULL) == 0);
   CHECK(atomic_load(&writer_b_entered));
   CHECK(scalar(c, "SELECT count(*) FROM file_contents WHERE content='writer B'") == 1);
   CHECK(scalar(c, "SELECT count(*) FROM (SELECT body FROM observer_events WHERE project='p' AND "
                   "state='ready' ORDER BY sequence DESC LIMIT 1) q WHERE body='writer B'") == 1);
   // A failed publication after SQL commit is reported failed, never ready.
   atomic_store(&fail_file, 1);
   code_index_file_data_t data = {0};
   data.content = "committed but not published";
   CHECK(kb_store_code_index_file_replace(file_id, &data) < 0);
   CHECK(scalar(c,
                "SELECT count(*) FROM file_contents WHERE content='committed but not published'") ==
         1);
   CHECK(scalar(c, "SELECT count(*) FROM (SELECT state FROM observer_events WHERE project='p' "
                   "ORDER BY sequence DESC LIMIT 1) q WHERE state='ready'") == 0);
   replace("recovered publication");
   // Connection/lease return rolls back and explicitly unlocks live scopes.
   {
      void *other = connect_owned();
      KB_INDEX_MUTATION(g);
      CHECK(kb_index_mutation_begin(&g, other, "p", "early_return", 0) == 0);
      sql(other, "BEGIN");
      sql(other, "UPDATE file_contents SET content='must rollback'");
      CHECK(kb_index_observer_connection_closing(other) == 0);
      aimee_pg_close(other);
      CHECK(kb_index_mutation_finish(&g) < 0);
   }
   CHECK(scalar(c, "SELECT count(*) FROM file_contents WHERE content='must rollback'") == 0);
   CHECK(
       scalar(
           c,
           "SELECT CASE WHEN pg_try_advisory_lock(hashtext('aimee_purge:p')) THEN 1 ELSE 0 END") ==
       1);
   sql(c, "SELECT pg_advisory_unlock(hashtext('aimee_purge:p'))");
   // A process dying after SQL commit cannot leave a session advisory lock or a
   // false ready callback behind. The fixture's building event is durable.
   pid_t child = fork();
   CHECK(child >= 0);
   if (child == 0)
   {
      void *own = connect_owned();
      KB_INDEX_MUTATION(g);
      CHECK(kb_index_mutation_begin(&g, own, "p", "crash", 0) == 0);
      sql(own, "UPDATE file_contents SET content='crash committed'");
      _exit(23);
   }
   int status;
   CHECK(waitpid(child, &status, 0) == child);
   CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 23);
   CHECK(scalar(c, "SELECT count(*) FROM file_contents WHERE content='crash committed'") == 1);
   CHECK(scalar(c, "SELECT count(*) FROM (SELECT state FROM observer_events WHERE project='p' "
                   "ORDER BY sequence DESC LIMIT 1) q WHERE state='building'") == 1);
   CHECK(
       scalar(
           c,
           "SELECT CASE WHEN pg_try_advisory_lock(hashtext('aimee_purge:p')) THEN 1 ELSE 0 END") ==
       1);
   sql(c, "SELECT pg_advisory_unlock(hashtext('aimee_purge:p'))");
   replace("recovered after crash");
   // Selected startup must fence EVERY existing project before any migration.
   CHECK(kb_store_code_index_project_upsert("Denied", "remote") > 0);
   denied_project = "Denied";
   {
      KB_INDEX_MUTATION(g);
      CHECK(kb_index_observer_startup_begin(&g, c) < 0);
   }
   denied_project = NULL;
   sql(c, "UPDATE kb_meta SET value='44' WHERE key='schema_version'");
   {
      KB_INDEX_MUTATION(g);
      CHECK(kb_index_observer_startup_begin(&g, c) < 0);
   }
   sql(c, "UPDATE kb_meta SET value='45' WHERE key='schema_version'");
   {
      KB_INDEX_MUTATION(g);
      CHECK(kb_index_observer_startup_begin(&g, c) == 0);
      CHECK(kb_index_mutation_finish(&g) == 0);
   }
   // The complete project set cannot change while the startup barrier owns
   // global source/catalog mutation. Project locks alone cannot fence new names.
   {
      KB_INDEX_MUTATION(startup);
      CHECK(kb_index_observer_startup_begin(&startup, c) == 0);
      pthread_t creator;
      CHECK(pthread_create(&creator, NULL, create_during_startup, NULL) == 0);
      while (!atomic_load(&creator_started))
      {
         struct timespec t = {0, 1000000};
         nanosleep(&t, NULL);
      }
      struct timespec t = {0, 200000000};
      nanosleep(&t, NULL);
      CHECK(!atomic_load(&creator_entered));
      CHECK(scalar(c, "SELECT count(*) FROM projects WHERE name='new_during_startup'") == 0);
      CHECK(kb_index_mutation_finish(&startup) == 0);
      CHECK(!atomic_load(&creator_entered));
      kb_index_mutation_cleanup(&startup);
      CHECK(pthread_join(creator, NULL) == 0);
      CHECK(atomic_load(&creator_entered));
   }
   // A malformed legacy path must not disappear behind the pagination cursor.
   {
      KB_INDEX_MUTATION(g);
      CHECK(kb_index_mutation_begin(&g, c, "p", "invalid_legacy_path", 0) == 0);
      int64_t id = scalar(c, "SELECT id FROM projects WHERE name='p'");
      int64_t invalid = kb_store_code_index_file_upsert(id, "", "2026-10-25T00:00:00Z");
      CHECK(invalid > 0);
      code_index_file_data_t invalid_data = {0};
      invalid_data.content = "invalid path evidence";
      CHECK(kb_store_code_index_file_replace(invalid, &invalid_data) == 0);
      CHECK(kb_index_mutation_finish(&g) < 0);
   }
   CHECK(kb_store_code_index_purge_files_matching(
             scalar(c, "SELECT id FROM projects WHERE name='p'"), "") >= 0);
   // Actual complete scan publication and its retractions use the same observer.
   CHECK(canonical_index_scan_begin("p", "remote", "observer-scan", NULL) == 0);
   canonical_index_file_input_t input = {.rel_path = "new.txt", .content = "new complete snapshot"};
   int accepted = 0;
   CHECK(canonical_index_scan_stage("observer-scan", &input, 1, &accepted) == 0 && accepted == 1);
   canonical_index_seal_result_t seal;
   CHECK(canonical_index_scan_seal("observer-scan", 1, &seal) == 0);
   CHECK(scalar(c, "SELECT count(*) FROM files f JOIN projects p ON p.id=f.project_id WHERE "
                   "p.name='p' AND f.path='main.txt'") == 0);
   int64_t p_id = scalar(c, "SELECT id FROM projects WHERE name='p'");
   CHECK(kb_store_code_index_purge_files_matching(p_id, "new%") >= 0);
   CHECK(kb_store_code_index_purge_hidden_except_manifests(p_id) >= 0);
   CHECK(kb_store_code_index_purge_hidden_pollution() >= 0);
   int64_t detached;
   CHECK(kb_store_code_project_detach("p", "fixture-operator", &detached) == 0);
   CHECK(detached == 1);
   CHECK(kb_store_code_index_project_upsert("p", "remote") > 0);
   CHECK(kb_store_code_index_project_current_generation("p", &generation) == 0 && generation == 2);
   CHECK(kb_store_code_index_project_delete("p") >= 0);
   CHECK(scalar(c, "SELECT count(*) FROM (SELECT state FROM observer_events WHERE project='p' "
                   "ORDER BY sequence DESC LIMIT 1) q WHERE state='ready'") == 0);
   project_boundary_tests(c);
   CHECK(aimee_kb_index_observer_register(NULL, NULL) == 0);
   CHECK(kb_store_code_index_project_upsert("unregistered_selected", "remote") < 0);
   CHECK(scalar(c, "SELECT count(*) FROM projects WHERE name='unregistered_selected'") == 0);
   kb_store_shutdown();
   puts("PASS real PostgreSQL observer: nested mutation, two writers/post-COMMIT lock, refusal, "
        "failed publication, lease cleanup, crash, complete startup, scan retraction, purge, "
        "detach/re-attach, delete, readonly isolation");
   return 0;
}
