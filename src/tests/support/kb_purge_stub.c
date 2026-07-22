/* kb_purge_stub.c — slice-2 kb purge wrapper stubs for test binaries that link
 * the real git_project.o but not kb_client.o (unit-test-git-project). Default
 * behavior is a TRANSPORT failure ({"status":"error"}), which exercises the
 * delete route's 503-abort path. AIMEE_TEST_KB_PURGE_MODE selects other
 * shapes:
 *   ok              — everything succeeds (purge ok:true, heartbeat
 *                     refreshed:true, finalize/cancel cleared:true);
 *   cancel-mismatch — the purge reaches the kb but a store fails (ok:false,
 *                     fence written) and the cancel is a generation-mismatch
 *                     no-op (cleared:false): the delete must take the
 *                     terminal purge-committed-unfinished path and must NOT
 *                     restore the holder count;
 *   hb-lost         — the purge succeeds but the first heartbeat reports
 *                     refreshed:false (a takeover displaced the operation):
 *                     the walk must stop with the fence retained.
 * AIMEE_TEST_CODE_INDEX_DELETE_FAIL=1 makes the (weak) local lexical-index
 * delete fail, for the local-index-failed abort path. */
#include "kb_client.h"

#include <stdlib.h>
#include <string.h>

static char *dup_str(const char *s)
{
   size_t n = strlen(s) + 1;
   char *p = malloc(n);
   if (p)
      memcpy(p, s, n);
   return p;
}

static const char *stub_mode(void)
{
   const char *m = getenv("AIMEE_TEST_KB_PURGE_MODE");
   return m ? m : "";
}

#define STUB_TRANSPORT_ERR "{\"status\":\"error\",\"message\":\"stub\"}"

char *kb_client_purge_project_json(const char *project, const char *generation,
                                   const char *purge_id, int takeover)
{
   (void)project;
   (void)generation;
   (void)purge_id;
   (void)takeover;
   const char *m = stub_mode();
   if (strcmp(m, "ok") == 0 || strcmp(m, "hb-lost") == 0)
      return dup_str(
          "{\"status\":\"ok\",\"ok\":true,\"stores\":{\"stub\":0},\"fence_replaced\":false}");
   if (strcmp(m, "cancel-mismatch") == 0)
      return dup_str("{\"status\":\"ok\",\"ok\":false,\"stores\":{\"stub\":{\"error\":\"boom\"}},"
                     "\"fence_replaced\":false}");
   return dup_str(STUB_TRANSPORT_ERR);
}

char *kb_client_purge_heartbeat_json(const char *project, const char *generation,
                                     const char *purge_id)
{
   (void)project;
   (void)generation;
   (void)purge_id;
   const char *m = stub_mode();
   if (strcmp(m, "ok") == 0 || strcmp(m, "cancel-mismatch") == 0)
      return dup_str("{\"status\":\"ok\",\"refreshed\":true}");
   if (strcmp(m, "hb-lost") == 0)
      return dup_str("{\"status\":\"ok\",\"refreshed\":false}");
   return dup_str(STUB_TRANSPORT_ERR);
}

char *kb_client_purge_finalize_json(const char *project, const char *generation,
                                    const char *purge_id)
{
   (void)project;
   (void)generation;
   (void)purge_id;
   const char *m = stub_mode();
   if (strcmp(m, "ok") == 0 || strcmp(m, "hb-lost") == 0)
      return dup_str("{\"status\":\"ok\",\"cleared\":true}");
   if (strcmp(m, "cancel-mismatch") == 0)
      return dup_str("{\"status\":\"ok\",\"cleared\":false}");
   return dup_str(STUB_TRANSPORT_ERR);
}

char *kb_client_purge_cancel_json(const char *project, const char *generation, const char *purge_id)
{
   (void)project;
   (void)generation;
   (void)purge_id;
   const char *m = stub_mode();
   if (strcmp(m, "ok") == 0 || strcmp(m, "hb-lost") == 0)
      return dup_str("{\"status\":\"ok\",\"cleared\":true}");
   if (strcmp(m, "cancel-mismatch") == 0)
      return dup_str("{\"status\":\"ok\",\"cleared\":false}"); /* mismatch no-op */
   return dup_str(STUB_TRANSPORT_ERR);
}

/* Weak so a future db2_test_shim.c definition (kb agent) wins without a
 * duplicate-symbol link failure. Success returns the DELETED ROW COUNT — a
 * positive 1 here, pinning that the delete flow treats any >= 0 as success
 * (the normal existing-project case deletes one projects row). */
__attribute__((weak)) int db2_code_index_project_delete(const char *name)
{
   (void)name;
   const char *fail = getenv("AIMEE_TEST_CODE_INDEX_DELETE_FAIL");
   return (fail && strcmp(fail, "1") == 0) ? -1 : 1;
}

/* STRONG override of git_project.c's weak local-index seam: the shipped
 * aimee-server flavor (AIMEE_DB2_DISABLED, which the test binary links) makes
 * it a 0-success no-op, so the failure path would be untestable without this
 * injection point. */
int gp_local_index_delete(const char *ref)
{
   (void)ref;
   const char *fail = getenv("AIMEE_TEST_CODE_INDEX_DELETE_FAIL");
   return (fail && strcmp(fail, "1") == 0) ? -1 : 1;
}
