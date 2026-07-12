/* delegate_ephemeral_ws.c: server-side ephemeral workspace for a background
 * delegate whose detached (client-served) workspace has no live client. See
 * delegate_ephemeral_ws.h.
 *
 * Symlink policy: the only path components under our control are the
 * `delegate-ws` anchor and the `<deleg_id>` leaf. Both create and remove open
 * those components with O_NOFOLLOW so a symlink planted at EITHER (an ancestor
 * `delegate-ws` symlink or a leaf symlink) is refused rather than followed —
 * closing the ancestor-symlink escape a lexical prefix check cannot prevent. */
#define _GNU_SOURCE
#include "delegate_ephemeral_ws.h"

#include "aimee_home.h"
#include "platform_path.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* A deleg_id is server-generated (e.g. "deleg-105-1783801198975185376-10"), but
 * it is interpolated into a filesystem path, so validate defensively: non-empty,
 * <=128 chars, only [A-Za-z0-9._-], no separators, no "..", and no leading '.'
 * (covers "." / ".." / hidden names). */
static int deleg_id_is_safe(const char *id)
{
   if (!id || !id[0])
      return 0;
   size_t n = strlen(id);
   if (n > 128)
      return 0;
   if (id[0] == '.')
      return 0;
   if (strstr(id, ".."))
      return 0;
   for (size_t i = 0; i < n; i++)
   {
      unsigned char c = (unsigned char)id[i];
      if (!(isalnum(c) || c == '-' || c == '_' || c == '.'))
         return 0;
   }
   return 1;
}

/* Open <home>/delegate-ws as a directory fd, refusing to follow a symlink at the
 * `delegate-ws` component (O_NOFOLLOW). Returns the fd (caller closes) or -1.
 * `create` mkdir's the anchor first. */
static int open_anchor_fd(int create)
{
   const char *home = aimee_home();
   if (!home || !home[0])
      return -1;
   char base[1024];
   if (snprintf(base, sizeof(base), "%s/delegate-ws", home) >= (int)sizeof(base))
      return -1;
   if (create)
      (void)platform_mkdir_p(base, 0700); /* best-effort; open below is the gate */
   return open(base, O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
}

int delegate_ephemeral_ws_create(const char *deleg_id, char *out, size_t out_cap)
{
   if (out && out_cap > 0)
      out[0] = '\0';
   if (!out || out_cap == 0 || !deleg_id_is_safe(deleg_id))
      return -1;

   const char *home = aimee_home();
   if (!home || !home[0])
      return -1;

   /* Confirm the caller's buffer fits the full path BEFORE creating anything, so a
    * truncation return never leaves an orphaned dir on disk. */
   char path[1024];
   if (snprintf(path, sizeof(path), "%s/delegate-ws/%s", home, deleg_id) >= (int)sizeof(path))
      return -1;
   if (strlen(path) >= out_cap)
      return -1;

   /* Open the anchor without following a symlinked `delegate-ws`, then create and
    * open the leaf relative to it, again refusing to follow a symlink. */
   int anchor = open_anchor_fd(1);
   if (anchor < 0)
      return -1;

   (void)mkdirat(anchor, deleg_id, 0700); /* EEXIST tolerated; open below is the gate */
   int leaf = openat(anchor, deleg_id, O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
   if (leaf < 0)
   {
      close(anchor);
      return -1; /* leaf is a symlink or not a directory */
   }

   /* Best-effort note (fd-relative, no-follow): read-only tools (grep/git/ls) then
    * surface a clear signal instead of misleading empty results in an empty dir. */
   int nfd = openat(leaf, "AIMEE_WORKSPACE_NOTE.txt",
                    O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
   if (nfd >= 0)
   {
      static const char note[] =
          "This is a server-side ephemeral git workspace for a background aimee delegate.\n"
          "The dispatching client disconnected, so the client's repository is NOT present\n"
          "here -- file/shell tools run against this initially-empty checkout. A background\n"
          "code delegate that must edit the client tree needs the repo provisioned here.\n";
      (void)!write(nfd, note, sizeof(note) - 1);
      close(nfd);
   }

   /* Make the workspace a git checkout: aimee's write-guard permits file writes
    * only in a git checkout / worktree (cwd_is_git_checkout), so a plain dir would
    * block every edit. `git init` in the leaf via fchdir on the already-opened
    * no-follow fd (no path re-resolution -> no TOCTOU). Best-effort: if git is
    * missing the workspace still exists, only writes stay guarded. */
   pid_t pid = fork();
   if (pid == 0)
   {
      if (fchdir(leaf) != 0)
         _exit(127);
      int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
      if (devnull >= 0)
      {
         dup2(devnull, STDOUT_FILENO);
         dup2(devnull, STDERR_FILENO);
      }
      execlp("git", "git", "init", "-q", (char *)NULL);
      _exit(127);
   }
   if (pid > 0)
   {
      int status = 0;
      waitpid(pid, &status, 0);
   }

   close(leaf);
   close(anchor);
   snprintf(out, out_cap, "%s", path); /* fits: checked above */
   return 0;
}

/* Recursively delete everything reachable from directory fd `dfd` (which this
 * function closes), following NO symlinks: each entry is fstatat'd with
 * AT_SYMLINK_NOFOLLOW; sub-directories are descended via openat(O_NOFOLLOW) and
 * removed with unlinkat(AT_REMOVEDIR); files/symlinks are unlinked in place. */
static void purge_dir_fd(int dfd)
{
   DIR *d = fdopendir(dfd); /* takes ownership of dfd */
   if (!d)
   {
      close(dfd);
      return;
   }
   int here = dirfd(d);
   struct dirent *e;
   while ((e = readdir(d)) != NULL)
   {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
         continue;
      struct stat st;
      if (fstatat(here, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
         continue;
      if (S_ISDIR(st.st_mode))
      {
         int child = openat(here, e->d_name, O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
         if (child >= 0)
            purge_dir_fd(child); /* recurse (closes child) */
         unlinkat(here, e->d_name, AT_REMOVEDIR);
      }
      else
      {
         unlinkat(here, e->d_name, 0); /* file or symlink: remove the link itself */
      }
   }
   closedir(d); /* closes dfd */
}

void delegate_ephemeral_ws_remove(const char *path)
{
   if (!path || !path[0] || strstr(path, ".."))
      return;
   const char *home = aimee_home();
   if (!home || !home[0])
      return;
   char prefix[1088];
   if (snprintf(prefix, sizeof(prefix), "%s/delegate-ws/", home) >= (int)sizeof(prefix))
      return;
   if (strncmp(path, prefix, strlen(prefix)) != 0)
      return;
   const char *leaf = path + strlen(prefix);
   /* leaf must be exactly one safe component. */
   if (!deleg_id_is_safe(leaf) || strchr(leaf, '/'))
      return;

   /* Open the anchor without following a symlinked `delegate-ws` (refuses the
    * ancestor-symlink escape), then the leaf without following a symlink. */
   int anchor = open_anchor_fd(0);
   if (anchor < 0)
      return;
   int leaffd = openat(anchor, leaf, O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
   if (leaffd >= 0)
   {
      purge_dir_fd(leaffd); /* closes leaffd */
      unlinkat(anchor, leaf, AT_REMOVEDIR);
   }
   else
   {
      /* leaf is a symlink or not a directory: remove the link only, never follow. */
      unlinkat(anchor, leaf, 0);
   }
   close(anchor);
}
