/* server_active_project.c: resolve the caller's active project from the request
 * `cwd`.
 *
 * `workspace_repo_identity()` answers this by reading the working tree — the
 * manifest, the git remote, a persisted UUID. That works only when the server
 * shares a filesystem with the caller. A thin client talking to a remote
 * aimee-server sends its OWN absolute path, which does not exist inside the
 * server; every read fails and the caller sees `scope_required` no matter how
 * the workspace was registered. That made the default agent-facing calls
 * (index.find, kb.search) unusable in the remote topology.
 *
 * The server already knows each registered project's root, so when the
 * filesystem cannot answer, the path itself still can: match `cwd` against the
 * indexed roots. Filesystem identity stays first so the co-located case keeps
 * its exact previous behaviour, including the durable repo identity that a
 * path match cannot reproduce. */
#include "server.h"
#include "aimee.h"
#include "index.h"
#include "kb_client.h"
#include <aimee/workspace/workspace.h>
#include "modules/workspace/workspace_turn.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* A root prefixes cwd only on a path-component boundary: root /a/foo covers
 * /a/foo and /a/foo/bar, but never /a/foobar. */
static int active_project_root_covers(const char *root, const char *cwd)
{
   if (!root || !root[0] || !cwd || !cwd[0])
      return 0;
   size_t rlen = strlen(root);
   /* A trailing slash on the stored root would otherwise defeat the boundary
    * test below; ignore it so "/a/foo/" and "/a/foo" behave identically. */
   while (rlen > 1 && root[rlen - 1] == '/')
      rlen--;
   if (strncmp(cwd, root, rlen) != 0)
      return 0;
   if (cwd[rlen] == '\0')
      return 1;
   if (cwd[rlen] == '/')
      return 1;
   /* Root "/" covers every absolute path and has no boundary character. */
   if (rlen == 1 && root[0] == '/')
      return 1;
   return 0;
}

int server_active_project_match(const char *cwd, const project_info_t *projects, int count,
                                char *out, size_t outlen)
{
   if (out && outlen > 0)
      out[0] = '\0';
   if (!cwd || !cwd[0] || !projects || count <= 0 || !out || outlen == 0)
      return -1;

   /* Longest matching root wins, so a workspace nested inside another resolves
    * to the inner one rather than its parent. */
   const project_info_t *best = NULL;
   size_t best_len = 0;
   for (int i = 0; i < count; i++)
   {
      if (!projects[i].name[0] || !active_project_root_covers(projects[i].root, cwd))
         continue;
      size_t rlen = strlen(projects[i].root);
      if (!best || rlen > best_len)
      {
         best = &projects[i];
         best_len = rlen;
      }
   }
   if (!best)
      return -1;
   if (snprintf(out, outlen, "%s", best->name) < 0 || out[0] == '\0')
   {
      out[0] = '\0';
      return -1;
   }
   return 0;
}

int server_active_project_from_cwd(const char *cwd, char *out, size_t outlen)
{
   if (out && outlen > 0)
      out[0] = '\0';
   if (!cwd || !cwd[0] || !out || outlen == 0)
      return -1;

   /* A readable repository's manifest/forge/persisted UUID is its owner.
    * An old index label at this path must not substitute another identity. */
   char identity[MAX_PATH_LEN] = "";
   if (workspace_repo_identity(cwd, identity, sizeof identity, NULL, 0) == 0 && identity[0])
   {
      snprintf(out, outlen, "%s", identity);
      return 0;
   }
   /* A remote thin-client path may be unreadable here. Only then may the
    * existing registry root supply its recorded identity. */
   project_info_t projects[128];
   int count = kb_client_index_list(projects, (int)(sizeof projects / sizeof projects[0]));
   if (count > 0 && server_active_project_match(cwd, projects, count, out, outlen) == 0)
      return 0;
   out[0] = '\0';
   return -1;
}

/* Learning scopes name the approved repository, independently of whether an
 * index service has indexed it under that identity or another display label.
 * Resolve through the held resource; a raw client path grants no authority. */
int server_learning_project_from_cwd(const char *cwd, char *out, size_t outlen)
{
   if (out && outlen)
      out[0] = '\0';
   if (!cwd || !cwd[0] || !out || !outlen)
      return -1;
   int fd = -1;
   char identity[65];
   if (workspace_turn_source_identity(cwd, &fd, identity) || fd < 0)
   {
      if (fd >= 0)
         close(fd);
      return -1;
   }
#ifdef __linux__
   char held[128];
   snprintf(held, sizeof held, "/proc/%ld/fd/%d", (long)getpid(), fd);
   const char *source = held;
#else
   const char *source = cwd;
#endif
   int rc = workspace_repo_identity(source, out, outlen, NULL, 0);
   close(fd);
   if (rc || !out[0])
   {
      out[0] = '\0';
      return -1;
   }
   return 0;
}
