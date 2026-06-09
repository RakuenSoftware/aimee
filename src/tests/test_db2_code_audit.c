/* test_db2_code_audit.c: DB2 code-audit assembly helper regressions. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "db2/kb_service_backend.h"
#include "db2/db_postgres.h"

void *db2_conn(void)
{
   return NULL;
}

aimee_pg_stmt_t *aimee_pg_prepare(void *pg_conn, const char *sql, char *errbuf, size_t errlen)
{
   (void)pg_conn;
   (void)sql;
   (void)errbuf;
   (void)errlen;
   return NULL;
}

void aimee_pg_finalize(aimee_pg_stmt_t *stmt)
{
   (void)stmt;
}

aimee_pg_step_t aimee_pg_step(aimee_pg_stmt_t *stmt, char *errbuf, size_t errlen)
{
   (void)stmt;
   (void)errbuf;
   (void)errlen;
   return AIMEE_PG_DONE;
}

int aimee_pg_bind_int(aimee_pg_stmt_t *stmt, const char *name, int value)
{
   (void)stmt;
   (void)name;
   (void)value;
   return 0;
}

int aimee_pg_bind_text(aimee_pg_stmt_t *stmt, const char *name, const char *value)
{
   (void)stmt;
   (void)name;
   (void)value;
   return 0;
}

const char *aimee_pg_column_text(aimee_pg_stmt_t *stmt, int col)
{
   (void)stmt;
   (void)col;
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

int main(void)
{
   printf("db2_code_audit: ");
   test_edge_target_like();
   printf("edge_target_like OK\n");
   printf("all tests passed\n");
   return 0;
}
