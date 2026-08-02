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
#include "workspace_scan_indexed.h"

int server_workspace_scan_indexed(int rc, int skipped, int inspected, int files)
{
   /* The rule itself lives in workspace_scan_indexed.h so the CLI reaches the
    * same verdict without linking the server. */
   return workspace_scan_indexed(rc, skipped, inspected, files);
}
