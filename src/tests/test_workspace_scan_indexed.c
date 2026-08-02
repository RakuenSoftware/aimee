/* Does `workspace add` tell the truth about whether it indexed anything?
 *
 * Found on a clean managed install: `aimee workspace add <path>` printed
 *   indexed: kbtest
 * having indexed nothing at all. kb is handed a filesystem PATH, and aimee-server
 * and aimee-kb are separate containers with no shared volume — the path exists for
 * the server and is absent for kb. kb walked an empty directory, found no files,
 * and returned success (POST /v1/code/scan -> 200). The old test was
 * `rc == 0 && !skipped`, so zero files was indistinguishable from a clean index:
 * the user is told their repo is searchable, gets empty or unrelated results, and
 * nothing anywhere reports a failure. */

#include "server_state_internal.h"
#include "workspace_scan_indexed.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
   /* THE BUG: a scan that returned cleanly having visited nothing. */
   assert(server_workspace_scan_indexed(0, 0, 0, 0) == 0);

   /* A real index. */
   assert(server_workspace_scan_indexed(0, 0, 12, 12) == 1);

   /* ALREADY INDEXED AND UNCHANGED: nothing re-indexed, but the files were seen.
    * This is why the test is `inspected`, not `files` — calling this "not
    * indexed" would be its own wrong answer, and would make every second
    * `workspace add` report a failure. */
   assert(server_workspace_scan_indexed(0, 0, 12, 0) == 1);

   /* An older kb does not report `inspected` (documented as 0). Fall back to
    * `files` so a working older kb is not called broken. */
   assert(server_workspace_scan_indexed(0, 0, 0, 7) == 1);

   /* Transport failure and explicit skips stay not-indexed, as before. */
   assert(server_workspace_scan_indexed(-1, 0, 0, 0) == 0);
   assert(server_workspace_scan_indexed(-1, 0, 99, 99) == 0); /* rc wins over counts */
   assert(server_workspace_scan_indexed(0, 1, 0, 0) == 0);
   assert(server_workspace_scan_indexed(0, 1, 99, 99) == 0); /* skipped wins too */

   /* `workspace add` and `index scan` must reach the SAME verdict about the same
    * scan. They did not: add warned that kb had seen no files while scan printed
    * a bare "Scan complete: 1 project(s), 0 file(s) re-indexed", so one broken
    * state read as a failure through one command and a success through the
    * other. The rule now lives in a shared header that both link; this pins that
    * the server entry point is that rule and not a second copy of it. */
   for (int rc = -1; rc <= 0; rc++)
      for (int skipped = 0; skipped <= 1; skipped++)
         for (int inspected = 0; inspected <= 3; inspected++)
            for (int files = 0; files <= 3; files++)
               assert(server_workspace_scan_indexed(rc, skipped, inspected, files) ==
                      workspace_scan_indexed(rc, skipped, inspected, files));

   /* The one sentence both surfaces show must actually say what to look at. */
   assert(WORKSPACE_SCAN_EMPTY_REASON[0] != '\0');

   printf("workspace scan indexed: ok\n");
   return 0;
}
