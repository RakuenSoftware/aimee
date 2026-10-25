/* Generic committed code-source observer. No memory/database-product policy. */
#include "index_observer_internal.h"
#include "db_postgres.h"
#include "db_schema.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define OBS_MAX_PROJECTS   256
#define OBS_MAX_FILES      100000
#define OBS_MAX_FILE_BYTES (64u * 1024u * 1024u)
struct scope
{
   int group, closed, failed, published, locked, stage, catalog_locked;
   unsigned refs;
   void *connection, *handle, *context;
   char *project;
   int64_t generation, lock_key;
   aimee_kb_index_observer_t observer;
   struct scope *next;
   kb_index_mutation_t *children;
   size_t count;
};
static pthread_mutex_t registration_lock = PTHREAD_MUTEX_INITIALIZER;
static aimee_kb_index_observer_t registration;
static void *registration_context;
static unsigned active;
static atomic_int required;
static _Thread_local struct scope *scopes;
static _Thread_local void *poisoned_connection;
static _Thread_local void *startup_connection;
static _Thread_local struct scope *startup_scope;

void aimee_kb_index_observer_require(void)
{
   atomic_store(&required, 1);
}
int aimee_kb_index_observer_required(void)
{
   return atomic_load(&required);
}
int aimee_kb_index_observer_register(const aimee_kb_index_observer_t *value, void *context)
{
   if (value && (value->abi_version != 1 || !value->available || !value->begin || !value->file ||
                 !value->ready || !value->unavailable || !value->release))
      return -1;
   pthread_mutex_lock(&registration_lock);
   if (active)
   {
      pthread_mutex_unlock(&registration_lock);
      return -1;
   }
   if (value)
      registration = *value;
   else
      memset(&registration, 0, sizeof(registration));
   registration_context = context;
   pthread_mutex_unlock(&registration_lock);
   return 0;
}
static int installed(void)
{
   pthread_mutex_lock(&registration_lock);
   int enabled = registration.abi_version != 0 || atomic_load(&required);
   pthread_mutex_unlock(&registration_lock);
   return enabled;
}
static int lock_project(void *connection, const char *project, int unlock)
{
   if (aimee_pg_is_shim())
      return -1; /* selected qualification requires real sessions */
   char error[256] = "";
   aimee_pg_stmt_t *q = aimee_pg_prepare(
       connection,
       unlock
           ? "SELECT CASE WHEN pg_advisory_unlock(hashtext('aimee_purge:' || ?1)) THEN 1 ELSE 0 END"
           : "SELECT pg_advisory_lock(hashtext('aimee_purge:' || ?1))",
       error, sizeof(error));
   if (!q)
      return -1;
   aimee_pg_bind_text(q, "?1", project);
   int ok = aimee_pg_step(q, error, sizeof(error)) == AIMEE_PG_ROW;
   if (ok && unlock)
      ok = aimee_pg_column_int(q, 0) == 1;
   aimee_pg_finalize(q);
   return ok ? 0 : -1;
}
static int lock_catalog(void *connection, int exclusive, int unlock)
{
   char error[256] = "";
   const char *sql =
       unlock
           ? (exclusive
                  ? "SELECT CASE WHEN pg_advisory_unlock(1095322963,1229866072) THEN 1 ELSE 0 END"
                  : "SELECT CASE WHEN pg_advisory_unlock_shared(1095322963,1229866072) THEN 1 ELSE "
                    "0 END")
           : (exclusive ? "SELECT pg_advisory_lock(1095322963,1229866072)"
                        : "SELECT pg_advisory_lock_shared(1095322963,1229866072)");
   if (aimee_pg_is_shim())
      return -1;
   aimee_pg_stmt_t *q = aimee_pg_prepare(connection, sql, error, sizeof(error));
   if (!q)
      return -1;
   int ok = aimee_pg_step(q, error, sizeof(error)) == AIMEE_PG_ROW;
   if (ok && unlock)
      ok = aimee_pg_column_int(q, 0) == 1;
   aimee_pg_finalize(q);
   return ok ? 0 : -1;
}
static int project_lock_key(void *connection, const char *project, int64_t *key)
{
   char error[256] = "";
   aimee_pg_stmt_t *q = aimee_pg_prepare(
       connection, "SELECT hashtext('aimee_purge:' || ?1)::bigint", error, sizeof(error));
   if (!q)
      return -1;
   aimee_pg_bind_text(q, "?1", project);
   int ok = aimee_pg_step(q, error, sizeof(error)) == AIMEE_PG_ROW;
   if (ok)
      *key = aimee_pg_column_int64(q, 0);
   aimee_pg_finalize(q);
   return ok ? 0 : -1;
}
static int project_state(void *connection, const char *project, int64_t *generation, char state[32])
{
   char error[256] = "";
   aimee_pg_stmt_t *q = aimee_pg_prepare(
       connection, "SELECT current_generation,lifecycle_state FROM projects WHERE name=?1", error,
       sizeof(error));
   if (!q)
      return -1;
   aimee_pg_bind_text(q, "?1", project);
   aimee_pg_step_t rc = aimee_pg_step(q, error, sizeof(error));
   if (rc == AIMEE_PG_ROW)
   {
      *generation = aimee_pg_column_int64(q, 0);
      const char *text = aimee_pg_column_text(q, 1);
      if (!text || strlen(text) >= 32 || *generation <= 0)
         rc = AIMEE_PG_ERR;
      else
         snprintf(state, 32, "%s", text);
   }
   aimee_pg_finalize(q);
   return rc == AIMEE_PG_ROW ? 1 : rc == AIMEE_PG_DONE ? 0 : -1;
}
static void close_scope(struct scope *scope)
{
   if (scope->closed)
      return;
   scope->closed = 1;
   if (!scope->published && scope->handle)
      (void)scope->observer.unavailable(scope->context, scope->handle, "failed");
   if (scope->handle)
      scope->observer.release(scope->context, scope->handle);
   scope->handle = NULL;
   if (scope->locked)
   {
      char error[256] = "";
      if (aimee_pg_in_transaction(scope->connection))
         (void)aimee_pg_exec(scope->connection, "ROLLBACK", error, sizeof(error));
      if (lock_project(scope->connection, scope->project, 1) != 0)
         poisoned_connection = scope->connection;
      scope->locked = 0;
   }
   if (scope->catalog_locked)
   {
      if (lock_catalog(scope->connection, 0, 1) != 0)
         poisoned_connection = scope->connection;
      scope->catalog_locked = 0;
   }
   struct scope **at = &scopes;
   while (*at && *at != scope)
      at = &(*at)->next;
   if (*at)
      *at = scope->next;
   pthread_mutex_lock(&registration_lock);
   if (active)
      active--;
   pthread_mutex_unlock(&registration_lock);
}
int kb_index_mutation_begin(kb_index_mutation_t *guard, void *connection, const char *project,
                            const char *operation, int may_reattach)
{
   if (!guard || guard->state || !connection || !project || !project[0] || strlen(project) > 1024)
      return -1;
   if (!installed())
      return 0;
   if (poisoned_connection)
      return -1;
   for (struct scope *at = scopes; at; at = at->next)
   {
      if (at->connection != connection)
         return -1;
      if (strcmp(at->project, project) == 0)
      {
         if (at->failed || at->closed || at->published || at->stage != 1)
            return -1;
         at->refs++;
         guard->state = at;
         return 0;
      }
   }
   int64_t lock_key;
   if (project_lock_key(connection, project, &lock_key) != 0)
      return -1;
   for (struct scope *at = scopes; at; at = at->next)
   {
      // Sort actual advisory keys, including hash collisions, not project labels.
      if (lock_key < at->lock_key || (lock_key == at->lock_key && strcmp(project, at->project) < 0))
         return -1;
   }
   if (aimee_pg_in_transaction(connection))
      return -1; /* before all row locks */
   struct scope *scope = calloc(1, sizeof(*scope));
   if (!scope)
      return -1;
   scope->project = strdup(project);
   scope->connection = connection;
   scope->refs = 1;
   if (!scope->project)
   {
      free(scope);
      return -1;
   }
   pthread_mutex_lock(&registration_lock);
   if (!registration.abi_version)
   {
      pthread_mutex_unlock(&registration_lock);
      free(scope->project);
      free(scope);
      return -1;
   }
   scope->observer = registration;
   scope->context = registration_context;
   active++;
   pthread_mutex_unlock(&registration_lock);
   scope->next = scopes;
   scopes = scope;
   guard->state = scope;
   if (scope->observer.available(scope->context) != 0)
      goto failed;
   if (startup_connection != connection)
   {
      if (lock_catalog(connection, 0, 0) != 0)
         goto failed;
      scope->catalog_locked = 1;
   }
   if (lock_project(connection, project, 0) != 0)
      goto failed;
   scope->locked = 1;
   char state[32] = "";
   int64_t generation = 1;
   int found = project_state(connection, project, &generation, state);
   if (found < 0 ||
       (may_reattach && found && strcmp(state, "current") != 0 && generation == INT64_MAX))
      goto failed;
   if (may_reattach && found && strcmp(state, "current") != 0)
      generation++;
   scope->generation = generation;
   scope->lock_key = lock_key;
   if (scope->observer.begin(scope->context, project, generation, operation, &scope->handle) != 0 ||
       !scope->handle)
      goto failed;
   scope->stage = 1;
   return 0;
failed:
   scope->failed = 1;
   kb_index_mutation_cleanup(guard);
   return -1;
}
static int id_project(void *connection, int64_t id, int file, char project[1025])
{
   char error[256] = "";
   aimee_pg_stmt_t *q = aimee_pg_prepare(
       connection,
       file ? "SELECT p.name FROM projects p JOIN files f ON f.project_id=p.id WHERE f.id=?1"
            : "SELECT name FROM projects WHERE id=?1",
       error, sizeof(error));
   if (!q)
      return -1;
   aimee_pg_bind_int64(q, "?1", id);
   int ok = aimee_pg_step(q, error, sizeof(error)) == AIMEE_PG_ROW;
   const char *text = ok ? aimee_pg_column_text(q, 0) : NULL;
   if (!text || !text[0] || strlen(text) > 1024)
      ok = 0;
   if (ok)
      snprintf(project, 1025, "%s", text);
   aimee_pg_finalize(q);
   return ok ? 0 : -1;
}
int kb_index_mutation_project_id(kb_index_mutation_t *guard, void *connection, int64_t id,
                                 const char *op)
{
   if (!installed())
      return 0;
   char project[1025];
   if (id_project(connection, id, 0, project) != 0)
      return -1;
   return kb_index_mutation_begin(guard, connection, project, op, 0);
}
int kb_index_mutation_file_id(kb_index_mutation_t *guard, void *connection, int64_t id,
                              const char *op)
{
   if (!installed())
      return 0;
   char project[1025];
   if (id_project(connection, id, 1, project) != 0)
      return -1;
   return kb_index_mutation_begin(guard, connection, project, op, 0);
}
static int publish(struct scope *scope)
{
   if (scope->failed || scope->closed || scope->published ||
       aimee_pg_in_transaction(scope->connection))
      return -1;
   scope->stage = 2;
   int64_t generation = 0;
   char state[32] = "";
   int found = project_state(scope->connection, scope->project, &generation, state);
   if (found < 0)
      return -1;
   if (!found || strcmp(state, "current") != 0)
   {
      if (scope->observer.unavailable(scope->context, scope->handle, found ? state : "missing") !=
          0)
         return -1;
      scope->published = 1;
      return 0;
   }
   if (generation != scope->generation)
      return -1;
   char *after = strdup("");
   if (!after)
      return -1;
   size_t count = 0;
   int ok = 1;
   for (;;)
   {
      char error[256] = "";
      aimee_pg_stmt_t *q = aimee_pg_prepare(
          scope->connection,
          "SELECT f.path,c.content,c.file_id FROM files f JOIN projects p ON p.id=f.project_id "
          "LEFT JOIN file_contents c ON c.file_id=f.id WHERE p.name=?1 AND "
          "p.lifecycle_state='current' AND f.generation=p.current_generation AND (?2='' OR f.path "
          "COLLATE "
          "\"C\">?2 COLLATE \"C\") "
          "ORDER BY f.path COLLATE \"C\" LIMIT 1",
          error, sizeof(error));
      if (!q)
      {
         ok = 0;
         break;
      }
      aimee_pg_bind_text(q, "?1", scope->project);
      aimee_pg_bind_text(q, "?2", after);
      aimee_pg_step_t rc = aimee_pg_step(q, error, sizeof(error));
      if (rc == AIMEE_PG_DONE)
      {
         aimee_pg_finalize(q);
         break;
      }
      const char *path = rc == AIMEE_PG_ROW ? aimee_pg_column_text(q, 0) : NULL;
      const char *content = rc == AIMEE_PG_ROW ? aimee_pg_column_text(q, 1) : NULL;
      if (!path || !path[0] || strlen(path) > 4096 || !content ||
          aimee_pg_column_type(q, 2) == AIMEE_PG_VALUE_NULL ||
          strlen(content) > OBS_MAX_FILE_BYTES || ++count > OBS_MAX_FILES ||
          scope->observer.file(scope->context, scope->handle, path, content, strlen(content)) != 0)
      {
         aimee_pg_finalize(q);
         ok = 0;
         break;
      }
      char *next = strdup(path);
      aimee_pg_finalize(q);
      if (!next)
      {
         ok = 0;
         break;
      }
      free(after);
      after = next;
   }
   free(after);
   if (!ok || scope->observer.ready(scope->context, scope->handle, count) != 0)
      return -1;
   scope->published = 1;
   return 0;
}
static void close_group(struct scope *group)
{
   if (group->closed)
      return;
   group->closed = 1;
   for (size_t i = group->count; i > 0; i--)
   {
      struct scope *child = group->children[i - 1].state;
      if (child)
         close_scope(child);
   }
   if (group->catalog_locked)
   {
      char error[256] = "";
      if (aimee_pg_in_transaction(group->connection))
         (void)aimee_pg_exec(group->connection, "ROLLBACK", error, sizeof(error));
      if (lock_catalog(group->connection, 1, 1) != 0)
         poisoned_connection = group->connection;
      group->catalog_locked = 0;
   }
   if (startup_scope == group)
   {
      startup_scope = NULL;
      startup_connection = NULL;
   }
}
int kb_index_mutation_finish(kb_index_mutation_t *guard)
{
   if (!guard || guard->finished)
      return -1;
   struct scope *scope = guard->state;
   if (!scope)
   {
      guard->finished = 1;
      return 0;
   }
   if (scope->group)
   {
      if (scope->closed)
         return -1;
      for (size_t i = 0; i < scope->count; i++)
         if (kb_index_mutation_finish(&scope->children[i]) != 0)
            return -1;
      guard->finished = 1;
      return 0;
   }
   if (scope->closed || scope->failed)
      return -1;
   if (scope->refs == 1 && publish(scope) != 0)
   {
      scope->failed = 1;
      return -1;
   }
   guard->finished = 1;
   return 0;
}
void kb_index_mutation_cleanup(kb_index_mutation_t *guard)
{
   if (!guard || !guard->state)
      return;
   struct scope *scope = guard->state;
   guard->state = NULL;
   if (scope->group)
   {
      close_group(scope);
      for (size_t i = scope->count; i > 0; i--)
         kb_index_mutation_cleanup(&scope->children[i - 1]);
      free(scope->children);
      startup_connection = NULL;
      pthread_mutex_lock(&registration_lock);
      if (active)
         active--;
      pthread_mutex_unlock(&registration_lock);
      free(scope);
      return;
   }
   if (!guard->finished)
      scope->failed = 1;
   if (--scope->refs == 0)
   {
      close_scope(scope);
      free(scope->project);
      free(scope);
   }
}
int kb_index_observer_connection_closing(void *connection)
{
   struct scope *at = scopes;
   while (at)
   {
      struct scope *next = at->next;
      if (at->connection == connection)
      {
         at->failed = 1;
         close_scope(at);
      }
      at = next;
   }
   if (startup_scope && startup_scope->connection == connection)
      close_group(startup_scope);
   int poison = poisoned_connection == connection;
   if (poison)
      poisoned_connection = NULL;
   return poison;
}
int kb_index_observer_startup_begin(kb_index_mutation_t *guard, void *connection)
{
   if (!installed())
      return 0;
   if (!guard || guard->state || !connection)
      return -1;
   if (startup_connection == connection)
      return 0;
   if (startup_connection || scopes || aimee_pg_in_transaction(connection))
      return -1;
   struct scope *group = calloc(1, sizeof(*group));
   if (!group)
      return -1;
   group->group = 1;
   group->connection = connection;
   group->children = calloc(OBS_MAX_PROJECTS, sizeof(*group->children));
   if (!group->children)
   {
      free(group);
      return -1;
   }
   guard->state = group;
   pthread_mutex_lock(&registration_lock);
   active++;
   aimee_kb_index_observer_t cb = registration;
   void *ctx = registration_context;
   pthread_mutex_unlock(&registration_lock);
   if (!cb.available || cb.available(ctx) != 0 || lock_catalog(connection, 1, 0) != 0)
   {
      kb_index_mutation_cleanup(guard);
      return -1;
   }
   group->catalog_locked = 1;
   startup_scope = group;
   startup_connection = connection;
   char version_error[256] = "";
   aimee_pg_stmt_t *version =
       aimee_pg_prepare(connection, "SELECT value FROM kb_meta WHERE key='schema_version'",
                        version_error, sizeof(version_error));
   int current =
       version && aimee_pg_step(version, version_error, sizeof(version_error)) == AIMEE_PG_ROW;
   if (current)
   {
      char expected[32];
      snprintf(expected, sizeof(expected), "%d", AIMEE_KB_STORE_SCHEMA_VERSION);
      const char *value = aimee_pg_column_text(version, 0);
      current = value && strcmp(value, expected) == 0;
   }
   if (version)
      aimee_pg_finalize(version);
   if (!current)
   {
      kb_index_mutation_cleanup(guard);
      return -1;
   }

   char error[256] = "";
   aimee_pg_stmt_t *q =
       aimee_pg_prepare(connection,
                        "SELECT name,current_generation,lifecycle_state FROM projects ORDER BY "
                        "hashtext('aimee_purge:' || name),name COLLATE \"C\"",
                        error, sizeof(error));
   if (!q)
   {
      kb_index_mutation_cleanup(guard);
      return -1;
   }
   int ok = 1;
   aimee_pg_step_t rc;
   while ((rc = aimee_pg_step(q, error, sizeof(error))) == AIMEE_PG_ROW)
   {
      const char *project = aimee_pg_column_text(q, 0);
      if (group->count == OBS_MAX_PROJECTS || !project ||
          kb_index_mutation_begin(&group->children[group->count], connection, project, "startup",
                                  0) != 0)
      {
         ok = 0;
         break;
      }
      group->count++;
   }
   if (rc != AIMEE_PG_DONE)
      ok = 0;
   aimee_pg_finalize(q);
   /* Even an empty source requires a real configured producer readiness probe. */
   if (ok && group->count == 0)
   {
      pthread_mutex_lock(&registration_lock);
      aimee_kb_index_observer_t cb = registration;
      void *ctx = registration_context;
      active++;
      pthread_mutex_unlock(&registration_lock);
      ok = cb.available && cb.available(ctx) == 0;
      pthread_mutex_lock(&registration_lock);
      active--;
      pthread_mutex_unlock(&registration_lock);
   }
   if (!ok)
   {
      kb_index_mutation_cleanup(guard);
      return -1;
   }
   return 0;
}
