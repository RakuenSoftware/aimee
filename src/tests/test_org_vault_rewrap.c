#include "db2/db2_pool.h"
#include "db2/db_postgres.h"
#include "db2/org_vault_rewrap.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

struct aimee_pg_stmt
{
   int unused;
};

static int g_conn, g_returns, g_discards;
static const char *g_fail_command;
static const char *g_fail_state;

int db2_pool_active(void)
{
   return 1;
}
void *db2_pool_lease(int timeout_ms)
{
   (void)timeout_ms;
   return &g_conn;
}
void db2_pool_return(void *conn)
{
   assert(conn == &g_conn);
   g_returns++;
}
void db2_pool_discard(void *conn)
{
   assert(conn == &g_conn);
   g_discards++;
}

int aimee_pg_exec_sqlstate(void *conn, const char *sql, char state[6], char *err, size_t errlen)
{
   (void)conn;
   (void)err;
   (void)errlen;
   state[0] = '\0';
   if (g_fail_command && strstr(sql, g_fail_command))
   {
      snprintf(state, 6, "%s", g_fail_state);
      return -1;
   }
   return 0;
}

aimee_pg_stmt_t *aimee_pg_prepare(void *c, const char *s, char *e, size_t n)
{
   (void)c;
   (void)s;
   (void)e;
   (void)n;
   return NULL;
}
void aimee_pg_finalize(aimee_pg_stmt_t *s)
{
   (void)s;
}
aimee_pg_step_t aimee_pg_step(aimee_pg_stmt_t *s, char *e, size_t n)
{
   (void)s;
   (void)e;
   (void)n;
   return AIMEE_PG_ERR;
}
const char *aimee_pg_sqlstate(const aimee_pg_stmt_t *s)
{
   (void)s;
   return "XX000";
}
int aimee_pg_bind_int(aimee_pg_stmt_t *s, const char *n, int v)
{
   (void)s;
   (void)n;
   (void)v;
   return -1;
}
int aimee_pg_bind_int64(aimee_pg_stmt_t *s, const char *n, int64_t v)
{
   (void)s;
   (void)n;
   (void)v;
   return -1;
}
int aimee_pg_bind_text(aimee_pg_stmt_t *s, const char *n, const char *v)
{
   (void)s;
   (void)n;
   (void)v;
   return -1;
}
int aimee_pg_bind_blob(aimee_pg_stmt_t *s, const char *n, const void *v, int z)
{
   (void)s;
   (void)n;
   (void)v;
   (void)z;
   return -1;
}
int aimee_pg_column_is_null(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return 1;
}
int aimee_pg_column_bytes(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return 0;
}
const void *aimee_pg_column_blob(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return NULL;
}
const char *aimee_pg_column_text(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return NULL;
}
int64_t aimee_pg_column_int64(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return 0;
}

static void test_sqlstate_mapping(void)
{
   assert(db2_vault_rewrap_classify_sqlstate("P7C01") == DB2_VAULT_REWRAP_CONFLICT);
   assert(db2_vault_rewrap_classify_sqlstate("P7I01") == DB2_VAULT_REWRAP_INTEGRITY);
   assert(db2_vault_rewrap_classify_sqlstate("40001") == DB2_VAULT_REWRAP_TRANSIENT);
   assert(db2_vault_rewrap_classify_sqlstate("40P01") == DB2_VAULT_REWRAP_TRANSIENT);
   assert(db2_vault_rewrap_classify_sqlstate("55000") == DB2_VAULT_REWRAP_BUSY);
   assert(db2_vault_rewrap_classify_sqlstate(NULL) == DB2_VAULT_REWRAP_ERROR);
}

static void *wrong_thread_commit(void *p)
{
   db2_vault_rewrap_tx_t **tx = p;
   assert(db2_vault_rewrap_tx_commit(tx) == DB2_VAULT_REWRAP_INVALID);
   return NULL;
}

static void test_public_handle_phases(void)
{
   db2_vault_rewrap_tx_t *tx = NULL;
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   pthread_t other;
   assert(pthread_create(&other, NULL, wrong_thread_commit, &tx) == 0);
   assert(pthread_join(other, NULL) == 0);
   assert(tx != NULL);

   db2_vault_rewrap_snapshot_t summary;
   assert(db2_vault_rewrap_verify_summary(tx, NULL, 1, &summary) == DB2_VAULT_REWRAP_INVALID);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_INVALID);
   assert(tx != NULL);
   db2_vault_rewrap_tx_rollback(&tx);
   assert(tx == NULL && g_returns == 1);

   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   g_fail_command = "COMMIT";
   g_fail_state = "40001";
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_TRANSIENT);
   assert(tx == NULL && g_discards == 1);

   g_fail_command = "ROLLBACK";
   g_fail_state = "08006";
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   db2_vault_rewrap_tx_rollback(&tx);
   assert(tx == NULL && g_discards == 2);
}

int main(void)
{
   test_sqlstate_mapping();
   test_public_handle_phases();
   puts("org_vault_rewrap: public phase/SQLSTATE tests passed");
   return 0;
}
