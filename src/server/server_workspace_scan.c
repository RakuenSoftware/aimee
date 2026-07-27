/* server_workspace_scan.c: did a workspace scan actually index the project?
 *
 * Its own translation unit because it is PURE POLICY and needs to be testable
 * without a live kb, a server context or a socket. It decides what the user is
 * told about their own repository, and it was getting that wrong: kb is handed a
 * filesystem PATH, and in the managed topology aimee-server and aimee-kb are
 * separate containers with no shared volume, so a path that exists for the server
 * is absent for kb. kb then walks an empty directory, finds no files, and answers
 * success -- which was reported to the user as `indexed: <project>` for a project
 * where nothing had been indexed at all. */

#include "server_state_internal.h"

int server_workspace_scan_indexed(int rc, int skipped, int inspected, int files)
{
   if (rc != 0 || skipped)
      return 0;
   /* `inspected` is the right test, not `files`: a project already indexed and
    * unchanged legitimately reports files == 0 with inspected > 0, and calling
    * that "not indexed" would be its own wrong answer. inspected == 0 means kb
    * saw no files to consider. Older kb builds do not report inspected
    * (documented as 0), so fall back to files rather than calling a working
    * older kb broken. */
   int visited = inspected > 0 ? inspected : files;
   return visited > 0;
}
