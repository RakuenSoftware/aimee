#include "modules/db2/c/db2_internal.h"
#include "modules/db2/c/db_postgres.h"

#include <assert.h>
#include <sqlite3.h>
#include <stdio.h>

static sqlite3 *test_db;

sqlite3 *db2_shared_sqlite(void)
{
   return test_db;
}

int main(void)
{
   assert(sqlite3_open(":memory:", &test_db) == SQLITE_OK);

   char err[256] = "";
   aimee_pg_prepare_error_t kind = AIMEE_PG_PREPARE_OK;
   assert(!aimee_pg_prepare_ex(NULL, "SELECT 1", &kind, err, sizeof(err)) &&
          kind == AIMEE_PG_PREPARE_INVALID);
   kind = AIMEE_PG_PREPARE_OK;
   assert(!aimee_pg_prepare_ex(test_db, NULL, &kind, err, sizeof(err)) &&
          kind == AIMEE_PG_PREPARE_INVALID);
   kind = AIMEE_PG_PREPARE_OK;
   assert(!aimee_pg_prepare_ex(test_db, "SELECT FROM", &kind, err, sizeof(err)) &&
          kind == AIMEE_PG_PREPARE_INVALID);

   kind = AIMEE_PG_PREPARE_INVALID;
   aimee_pg_stmt_t *stmt = aimee_pg_prepare_ex(test_db, "SELECT 1", &kind, err, sizeof(err));
   assert(stmt && kind == AIMEE_PG_PREPARE_OK);
   aimee_pg_finalize(stmt);

   sqlite3_int64 old_limit = sqlite3_hard_heap_limit64(1);
   kind = AIMEE_PG_PREPARE_INVALID;
   stmt = aimee_pg_prepare_ex(test_db, "SELECT 1", &kind, err, sizeof(err));
   sqlite3_hard_heap_limit64(old_limit);
   assert(!stmt && kind == AIMEE_PG_PREPARE_RESOURCE);

   assert(sqlite3_close(test_db) == SQLITE_OK);
   test_db = NULL;
   puts("pg_prepare_classification: all tests passed");
   return 0;
}
