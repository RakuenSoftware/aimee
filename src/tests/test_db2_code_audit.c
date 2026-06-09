/* test_db2_code_audit.c: DB2 code-audit assembly helper regressions. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db2/kb_service_backend.h"
#include "db2/db_postgres.h"

typedef enum
{
   FAKE_STMT_NONE = 0,
   FAKE_STMT_EDGES,
   FAKE_STMT_CLONES,
   FAKE_STMT_NEAR_CLONES,
} fake_stmt_kind_t;

typedef struct
{
   fake_stmt_kind_t kind;
   int row;
   int limit;
   char relation[32];
   char project[128];
} fake_stmt_t;

typedef struct
{
   const char *relation;
   const char *source;
   const char *target;
} fake_edge_row_t;

typedef struct
{
   const char *body_hash;
   const char *symbol;
   const char *file_path;
   const char *payload_json;
} fake_clone_row_t;

static const fake_edge_row_t fake_edges[] = {
    {"exports", "src/a.c", "export:proj:Used"},
    {"exports", "src/b.c", "export:proj:Dead"},
    {"exports", "src/c.c", "export:proj:LoopA"},
    {"exports", "src/d.c", "export:proj:LoopB"},
    {"imports", "src/user.c", "import:proj:Used"},
    {"imports", "src/c.c", "import:proj:LoopB"},
    {"imports", "src/d.c", "import:proj:LoopA"},
    {"references", "src/ref.c", "reference:proj:NotAnExport"},
};

static const fake_clone_row_t fake_clones[] = {
    {"h1", "symA", "src/a.c", "{\"line_count\":6}"},
    {"h1", "symB", "src/b.c", "{\"line_count\":5}"},
    {"h2", "tinyA", "src/tiny1.c", "{\"line_count\":3}"},
    {"h2", "tinyB", "src/tiny2.c", "{\"line_count\":4}"},
    {"h3", "legacyA", "src/legacy1.c", "{}"},
    {"h3", "legacyB", "src/legacy2.c", ""},
    {"h4", "extraA", "src/extra1.c", "{\"line_count\":8}"},
    {"h4", "extraB", "src/extra2.c", "{\"line_count\":8}"},
};

void *db2_conn(void)
{
   return (void *)0x1;
}

aimee_pg_stmt_t *aimee_pg_prepare(void *pg_conn, const char *sql, char *errbuf, size_t errlen)
{
   (void)pg_conn;
   (void)errbuf;
   (void)errlen;
   fake_stmt_t *st = calloc(1, sizeof(*st));
   if (!st)
      return NULL;
   st->row = -1;
   st->limit = 1000000;
   if (strstr(sql, "FROM entity_edges"))
      st->kind = FAKE_STMT_EDGES;
   else if (strstr(sql, "FROM code_embeddings a JOIN LATERAL"))
      st->kind = FAKE_STMT_NEAR_CLONES;
   else if (strstr(sql, "FROM code_embeddings"))
      st->kind = FAKE_STMT_CLONES;
   else
      st->kind = FAKE_STMT_NONE;
   return (aimee_pg_stmt_t *)st;
}

void aimee_pg_finalize(aimee_pg_stmt_t *stmt)
{
   free(stmt);
}

aimee_pg_step_t aimee_pg_step(aimee_pg_stmt_t *stmt, char *errbuf, size_t errlen)
{
   (void)errbuf;
   (void)errlen;
   fake_stmt_t *st = (fake_stmt_t *)stmt;
   if (!st)
      return AIMEE_PG_DONE;
   if (st->kind == FAKE_STMT_EDGES)
   {
      for (int i = st->row + 1; i < (int)(sizeof(fake_edges) / sizeof(fake_edges[0])); i++)
      {
         if (st->relation[0] && strcmp(fake_edges[i].relation, st->relation) != 0)
            continue;
         st->row = i;
         return AIMEE_PG_ROW;
      }
      return AIMEE_PG_DONE;
   }
   if (st->kind == FAKE_STMT_CLONES)
   {
      st->row++;
      if (st->row < (int)(sizeof(fake_clones) / sizeof(fake_clones[0])) && st->row < st->limit)
         return AIMEE_PG_ROW;
   }
   return AIMEE_PG_DONE;
}

int aimee_pg_bind_int(aimee_pg_stmt_t *stmt, const char *name, int value)
{
   fake_stmt_t *st = (fake_stmt_t *)stmt;
   if (st && name && strcmp(name, "?4") == 0)
      st->limit = value;
   return 0;
}

int aimee_pg_bind_text(aimee_pg_stmt_t *stmt, const char *name, const char *value)
{
   fake_stmt_t *st = (fake_stmt_t *)stmt;
   if (!st || !name || !value)
      return 0;
   if (strcmp(name, "?1") == 0 && st->kind == FAKE_STMT_EDGES)
      snprintf(st->relation, sizeof(st->relation), "%s", value);
   if ((strcmp(name, "?1") == 0 && st->kind == FAKE_STMT_CLONES) ||
       (strcmp(name, "?2") == 0 && st->kind == FAKE_STMT_EDGES))
      snprintf(st->project, sizeof(st->project), "%s", value);
   return 0;
}

const char *aimee_pg_column_text(aimee_pg_stmt_t *stmt, int col)
{
   fake_stmt_t *st = (fake_stmt_t *)stmt;
   if (!st)
      return "";
   if (st->kind == FAKE_STMT_EDGES && st->row >= 0 &&
       st->row < (int)(sizeof(fake_edges) / sizeof(fake_edges[0])))
   {
      if (col == 0)
         return fake_edges[st->row].source;
      if (col == 1)
         return fake_edges[st->row].target;
   }
   if (st->kind == FAKE_STMT_CLONES && st->row >= 0 &&
       st->row < (int)(sizeof(fake_clones) / sizeof(fake_clones[0])))
   {
      if (col == 0)
         return fake_clones[st->row].body_hash;
      if (col == 1)
         return fake_clones[st->row].symbol;
      if (col == 2)
         return fake_clones[st->row].file_path;
      if (col == 3)
         return fake_clones[st->row].payload_json;
   }
   return "";
}

double aimee_pg_column_double(aimee_pg_stmt_t *stmt, int col)
{
   (void)stmt;
   (void)col;
   return 0.0;
}

static void test_edge_target_like(void)
{
   char like[256];
   assert(db2_code_audit_edge_target_like("exports", "proj", like, sizeof(like)) == 0);
   assert(strcmp(like, "export:proj:%") == 0);

   assert(db2_code_audit_edge_target_like("imports", "my project", like, sizeof(like)) == 0);
   assert(strcmp(like, "import:my%20project:%") == 0);

   assert(db2_code_audit_edge_target_like("references", "my project", like, sizeof(like)) == 0);
   assert(strcmp(like, "reference:my%20project:%") == 0);

   assert(db2_code_audit_edge_target_like("exports", "", like, sizeof(like)) == 0);
   assert(strcmp(like, "") == 0);
   assert(db2_code_audit_edge_target_like("defines", "proj", like, sizeof(like)) != 0);
}

static int array_contains_text(cJSON *arr, const char *needle)
{
   assert(cJSON_IsArray(arr));
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, arr)
   {
      if (cJSON_IsString(item) && item->valuestring && strstr(item->valuestring, needle))
         return 1;
   }
   return 0;
}

static int clone_groups_contain(cJSON *groups, const char *a, const char *b)
{
   assert(cJSON_IsArray(groups));
   cJSON *group = NULL;
   cJSON_ArrayForEach(group, groups)
   {
      if (array_contains_text(group, a) && array_contains_text(group, b))
         return 1;
   }
   return 0;
}

static void test_code_audit_json_indexed_project(void)
{
   cJSON *json = db2_kb_service_code_audit_json("proj", 2);
   assert(json);

   cJSON *status = cJSON_GetObjectItemCaseSensitive(json, "status");
   assert(cJSON_IsString(status));
   assert(strcmp(status->valuestring, "ok") == 0);

   cJSON *clone_min_lines = cJSON_GetObjectItemCaseSensitive(json, "clone_min_lines");
   assert(cJSON_IsNumber(clone_min_lines));
   assert(clone_min_lines->valueint == 5);

   cJSON *dead = cJSON_GetObjectItemCaseSensitive(json, "dead_exports");
   assert(cJSON_IsArray(dead));
   assert(cJSON_GetArraySize(dead) == 1);
   assert(array_contains_text(dead, "export:proj:Dead"));
   assert(!array_contains_text(dead, "export:proj:Used"));

   cJSON *cycles = cJSON_GetObjectItemCaseSensitive(json, "cycles");
   assert(cJSON_IsArray(cycles));
   assert(cJSON_GetArraySize(cycles) > 0);
   assert(cJSON_GetArraySize(cycles) <= 2);
   assert(array_contains_text(cycles, "src/c.c"));
   assert(array_contains_text(cycles, "src/d.c"));

   cJSON *clones = cJSON_GetObjectItemCaseSensitive(json, "clones");
   assert(cJSON_IsArray(clones));
   assert(cJSON_GetArraySize(clones) == 2);
   assert(clone_groups_contain(clones, "symA", "symB"));
   assert(clone_groups_contain(clones, "legacyA", "legacyB"));
   assert(!clone_groups_contain(clones, "tinyA", "tinyB"));
   assert(!clone_groups_contain(clones, "extraA", "extraB"));

   cJSON *near_clones = cJSON_GetObjectItemCaseSensitive(json, "near_clones");
   assert(cJSON_IsArray(near_clones));
   assert(cJSON_GetArraySize(near_clones) == 0);

   cJSON_Delete(json);
   printf("code_audit_json_indexed_project OK\n");
}

int main(void)
{
   printf("db2_code_audit: ");
   test_edge_target_like();
   printf("edge_target_like OK\n");
   test_code_audit_json_indexed_project();
   printf("all tests passed\n");
   return 0;
}
