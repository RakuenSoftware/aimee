/* delegate_ephemeral_ws.c: server-side ephemeral workspace for a background
 * delegate whose detached (client-served) workspace has no live client. See
 * delegate_ephemeral_ws.h. */
#include "delegate_ephemeral_ws.h"

#include "aimee_home.h"
#include "platform_path.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A deleg_id is server-generated (e.g. "deleg-105-1783801198975185376-10"), but
 * it is interpolated into a filesystem path, so validate defensively against
 * traversal / separators regardless of the trusted source. */
static int deleg_id_is_safe(const char *id)
{
   if (!id || !id[0])
      return 0;
   size_t n = strlen(id);
   if (n > 128)
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

int delegate_ephemeral_ws_create(const char *deleg_id, char *out, size_t out_cap)
{
   if (out && out_cap > 0)
      out[0] = '\0';
   if (!out || out_cap == 0 || !deleg_id_is_safe(deleg_id))
      return -1;

   const char *home = aimee_home();
   if (!home || !home[0])
      return -1;

   char path[1024];
   if (snprintf(path, sizeof(path), "%s/delegate-ws/%s", home, deleg_id) >= (int)sizeof(path))
      return -1;
   if (platform_mkdir_p(path, 0700) != 0)
      return -1;

   /* Best-effort note: read-only tools (grep/git/ls) then surface a clear signal
    * instead of misleading empty results in an otherwise-empty workspace. */
   char note[1088];
   if (snprintf(note, sizeof(note), "%s/AIMEE_WORKSPACE_NOTE.txt", path) < (int)sizeof(note))
   {
      FILE *f = fopen(note, "w");
      if (f)
      {
         fputs("This is a server-side ephemeral workspace for a background aimee delegate.\n"
               "The dispatching client disconnected, so the client's repository is NOT present\n"
               "here -- file/shell tools run against this empty directory. A background code\n"
               "delegate that must edit the client tree needs the repo provisioned server-side.\n",
               f);
         fclose(f);
      }
   }

   if ((size_t)snprintf(out, out_cap, "%s", path) >= out_cap)
   {
      out[0] = '\0';
      return -1;
   }
   return 0;
}

/* best-effort recursive delete */
static void ephemeral_rm_rf(const char *path)
{
   DIR *d = opendir(path);
   if (!d)
   {
      unlink(path);
      return;
   }
   struct dirent *e;
   while ((e = readdir(d)) != NULL)
   {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
         continue;
      char child[2048];
      if (snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >= (int)sizeof(child))
         continue;
      struct stat st;
      if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
         ephemeral_rm_rf(child);
      else
         unlink(child);
   }
   closedir(d);
   rmdir(path);
}

void delegate_ephemeral_ws_remove(const char *path)
{
   if (!path || !path[0] || strstr(path, ".."))
      return;
   const char *home = aimee_home();
   if (!home || !home[0])
      return;
   char prefix[1024];
   if (snprintf(prefix, sizeof(prefix), "%s/delegate-ws/", home) >= (int)sizeof(prefix))
      return;
   /* Safety: only remove paths that live under <home>/delegate-ws/. */
   if (strncmp(path, prefix, strlen(prefix)) != 0)
      return;
   ephemeral_rm_rf(path);
}
