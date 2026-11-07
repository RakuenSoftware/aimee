/* test_instance_identity.c: the first-boot identity latch and the role refs
 * each aimee instance refuses on its ring. Moved with the code from the ring's
 * runtime test (2026-10-27). */
#define _GNU_SOURCE
#include <aimee/audit/instance_identity.h>

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static void test_identity_latch(const char *directory)
{
   char path[PATH_MAX];
   snprintf(path, sizeof path, "%s/instance-identity.json", directory);
   assert(aimee_instance_ensure_identity(directory, AIMEE_INSTANCE_SERVER, "/nonexistent") != 0);
   FILE *f = fopen(path, "w");
   assert(f);
   const char *record =
       "{\"version\":1,\"role\":\"server\",\"id\":\"dff6b808-0888-41db-b6fc-fc4077dbe3a2\"}\n";
   assert(fputs(record, f) >= 0);
   assert(fclose(f) == 0);
   assert(aimee_instance_ensure_identity(directory, AIMEE_INSTANCE_SERVER, "/nonexistent") != 0);
   assert(chmod(path, 0444) == 0);
   assert(aimee_instance_ensure_identity(directory, AIMEE_INSTANCE_SERVER, "/nonexistent") == 0);
   assert(aimee_instance_ensure_identity(directory, AIMEE_INSTANCE_KB, "/nonexistent") != 0);
   assert(aimee_instance_ensure_identity(directory, AIMEE_INSTANCE_UNSET, NULL) != 0);
   assert(chmod(path, 0644) == 0);
   f = fopen(path, "a");
   assert(f && fputs("trailing", f) >= 0 && fclose(f) == 0);
   assert(chmod(path, 0444) == 0);
   assert(aimee_instance_ensure_identity(directory, AIMEE_INSTANCE_SERVER, NULL) != 0);
   assert(unlink(path) == 0);
   assert(symlink("/nonexistent", path) == 0);
   assert(aimee_instance_ensure_identity(directory, AIMEE_INSTANCE_SERVER, NULL) != 0);
   assert(unlink(path) == 0);
}

static void test_refused_role_refs(void)
{
   size_t n = 0;
   const uint32_t *refs = aimee_instance_refused_role_refs(AIMEE_INSTANCE_SERVER, &n);
   assert(n == 1 && refs[0] == AIMEE_KB_ROLE_REF);
   refs = aimee_instance_refused_role_refs(AIMEE_INSTANCE_KB, &n);
   assert(n == 1 && refs[0] == AIMEE_SERVER_ROLE_REF);
   refs = aimee_instance_refused_role_refs(AIMEE_INSTANCE_UNSET, &n);
   assert(n == 2 && refs[0] == AIMEE_SERVER_ROLE_REF && refs[1] == AIMEE_KB_ROLE_REF);
}

int main(void)
{
   char directory[256];
   snprintf(directory, sizeof directory, "%s/aimee-instance-identity-XXXXXX", platform_tmpdir());
   assert(mkdtemp(directory) != NULL);
   test_identity_latch(directory);
   test_refused_role_refs();
   assert(rmdir(directory) == 0);
   puts("instance identity: latch validation and refused role refs passed");
   return 0;
}
