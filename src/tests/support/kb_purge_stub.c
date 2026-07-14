/* kb_purge_stub.c — slice-2 kb purge wrapper stubs for test binaries that link
 * the real git_project.o but not kb_client.o (unit-test-git-project). Default
 * behavior is a TRANSPORT failure ({"status":"error"}), which exercises the
 * delete route's 503-abort path; setting AIMEE_TEST_KB_PURGE_MODE=ok makes the
 * purge succeed so the purged/finalize path is testable too. Also stubs the
 * server-local lexical index delete (db2_code_index_project_delete), which the
 * kb agent implements in db2/. */
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

static int stub_ok_mode(void)
{
   const char *m = getenv("AIMEE_TEST_KB_PURGE_MODE");
   return m && strcmp(m, "ok") == 0;
}

char *kb_client_purge_project_json(const char *project, const char *generation,
                                   const char *purge_id, int takeover)
{
   (void)project;
   (void)generation;
   (void)purge_id;
   (void)takeover;
   if (stub_ok_mode())
      return dup_str(
          "{\"status\":\"ok\",\"ok\":true,\"stores\":{\"stub\":0},\"fence_replaced\":false}");
   return dup_str("{\"status\":\"error\",\"message\":\"stub\"}");
}

char *kb_client_purge_heartbeat_json(const char *project, const char *generation,
                                     const char *purge_id)
{
   (void)project;
   (void)generation;
   (void)purge_id;
   if (stub_ok_mode())
      return dup_str("{\"status\":\"ok\"}");
   return dup_str("{\"status\":\"error\",\"message\":\"stub\"}");
}

char *kb_client_purge_finalize_json(const char *project, const char *generation,
                                    const char *purge_id)
{
   (void)project;
   (void)generation;
   (void)purge_id;
   if (stub_ok_mode())
      return dup_str("{\"status\":\"ok\",\"cleared\":true}");
   return dup_str("{\"status\":\"error\",\"message\":\"stub\"}");
}

char *kb_client_purge_cancel_json(const char *project, const char *generation, const char *purge_id)
{
   (void)project;
   (void)generation;
   (void)purge_id;
   if (stub_ok_mode())
      return dup_str("{\"status\":\"ok\",\"cleared\":true}");
   return dup_str("{\"status\":\"error\",\"message\":\"stub\"}");
}

/* Weak so a future db2_test_shim.c definition (kb agent) wins without a
 * duplicate-symbol link failure. */
__attribute__((weak)) int db2_code_index_project_delete(const char *name)
{
   (void)name;
   return 0;
}
