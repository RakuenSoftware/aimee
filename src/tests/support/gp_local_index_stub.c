/* gp_local_index_stub.c — strong override of git_project.c's weak
 * gp_local_index_delete seam, so a test can inject a failure and assert the
 * delete path aborts BEFORE any filesystem removal.
 *
 * Returns -1 when AIMEE_TEST_CODE_INDEX_DELETE_FAIL=1, else a success row count.
 * (Split out of the former kb_purge_stub.c, whose kb purge wrappers went away
 * with the delete path's kb calls — this seam is unrelated to them.) */
#include <stdlib.h>
#include <string.h>

int gp_local_index_delete(const char *ref);

int gp_local_index_delete(const char *ref)
{
   (void)ref;
   const char *fail = getenv("AIMEE_TEST_CODE_INDEX_DELETE_FAIL");
   return (fail && strcmp(fail, "1") == 0) ? -1 : 1;
}
