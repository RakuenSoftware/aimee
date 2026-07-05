/* test_kb_audit_worm.c: the aimee-kb Postgres WORM store (S5), against the DB2
 * SQLite shim. Verifies append/chain/count, WORM triggers, tamper detection, and
 * that the row hash is BYTE-IDENTICAL to the aimee-server store (shared
 * audit_worm_chain — the cross-engine vector). */
#include "../db2/db2_test_shim.h"
#include "audit_worm_chain.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "kb_audit_worm.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_append_and_verify(void)
{
   assert(db2_kb_audit_append("primary", "u", "tool.read", "v1-1", "allow", "{}") == 0);
   assert(db2_kb_audit_append("delegate", "mimo", "kb.query", "q1", "ok", "{}") == 0);
   assert(db2_kb_audit_count() == 2);
   char err[160];
   assert(db2_kb_audit_verify_chain(err, sizeof err) == 0);
   printf("  PASS: kb_append_and_verify\n");
}

/* The kb (Postgres) store and the server (SQLite) store hash a row identically
 * because both call audit_worm_row_hash. Pin the genesis-row value so a
 * canonicalization change to either engine is caught. */
static void test_cross_engine_vector(void)
{
   char h[65];
   audit_worm_row_hash(1, "primary", "u", "tool.read", "v1-1", "allow", "", "{}",
                       AUDIT_WORM_GENESIS_PREV, h);
   /* Pinned cross-engine vector: the aimee-server SQLite store computes exactly
    * this for the same fields (both call audit_worm_row_hash). A canonicalization
    * change to either engine breaks this. */
   assert(strcmp(h, "3c2adf68ae8f1b704780ffedd32522e06468e34a69205240fbb358a7122ff986") == 0);
   printf("  PASS: kb_cross_engine_vector (%s)\n", h);
}

/* The WORM triggers reject UPDATE/DELETE of a committed row. */
static void test_worm_triggers(void)
{
   db2_kb_audit_append("primary", "u", "tool.read", "x", "allow", "{}");
   char err[160];
   int urc = aimee_pg_exec(db2_conn(), "UPDATE kb_audit_event SET verdict='block' WHERE seq=1", err,
                           sizeof err);
   assert(urc != 0); /* trigger RAISE / append-only */
   int drc = aimee_pg_exec(db2_conn(), "DELETE FROM kb_audit_event WHERE seq=1", err, sizeof err);
   assert(drc != 0);
   printf("  PASS: kb_worm_triggers\n");
}

/* Tampering past the triggers (drop them) is still caught by the chain. */
static void test_tamper_detected(void)
{
   db2_kb_audit_append("primary", "u", "tool.read", "t1", "allow", "{}");
   db2_kb_audit_append("primary", "u", "tool.read", "t2", "allow", "{}");
   assert(db2_kb_audit_verify_chain(NULL, 0) == 0);
   char err[160];
   aimee_pg_exec(db2_conn(), "DROP TRIGGER kb_audit_no_update", err, sizeof err);
   aimee_pg_exec(db2_conn(), "DROP TRIGGER kb_audit_no_delete", err, sizeof err);
   assert(aimee_pg_exec(db2_conn(), "UPDATE kb_audit_event SET subject='EVIL' WHERE seq=1", err,
                        sizeof err) == 0);
   assert(db2_kb_audit_verify_chain(err, sizeof err) == -1);
   assert(strstr(err, "seq 1") != NULL);
   printf("  PASS: kb_tamper_detected (%s)\n", err);
}

int main(void)
{
   db2_test_shim_open();
   test_append_and_verify();
   test_cross_engine_vector();
   test_worm_triggers();
   test_tamper_detected();
   db2_test_shim_close();
   printf("kb_audit_worm: all tests passed\n");
   return 0;
}
