/* windows/workspace_provider.c: the `shared` (local-fs) resource provider for
 * the Windows thin client.
 *
 * When the client serves a `detached` workspace to a remote aimee-server (via
 * `aimee workspace serve` or the auto-started reverse-channel), the server
 * marshals each file/exec op back here; ws_detached_runner_handle executes it
 * through workspace_provider_shared(). This is the Windows implementation of
 * that provider — the Win32/CRT counterpart of posix/workspace_provider.c, so a
 * Windows client serves its working tree exactly like a POSIX one.
 *
 * read_all/write_all are plain stdio; stat/list/exec use the Win32 CRT
 * (_stat, _findfirst, _popen). exec_shell runs the command through the native
 * shell (cmd.exe via _popen), honouring the runner's thread-local cwd. */
#include "workspace_provider.h"

#include <direct.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Thread-local cwd the runner sets (run_cmd_set_cwd) before an exec op, shared
 * with util.c. NULL when unset. */
extern const char *run_cmd_get_cwd(void);

static int shared_read_all(const workspace_provider_t *p, const char *path, char **out, size_t *len)
{
   (void)p;
   if (out)
      *out = NULL;
   if (len)
      *len = 0;
   if (!path || !out)
      return -1;

   FILE *f = fopen(path, "rb");
   if (!f)
      return -1;
   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      return -1;
   }
   long sz = ftell(f);
   if (sz < 0 || fseek(f, 0, SEEK_SET) != 0)
   {
      fclose(f);
      return -1;
   }
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return -1;
   }
   size_t rd = fread(buf, 1, (size_t)sz, f);
   if (ferror(f))
   {
      free(buf);
      fclose(f);
      return -1;
   }
   fclose(f);
   buf[rd] = '\0';
   *out = buf;
   if (len)
      *len = rd;
   return 0;
}

static int shared_write_all(const workspace_provider_t *p, const char *path, const char *data,
                            size_t len)
{
   (void)p;
   if (!path)
      return -1;

   FILE *f = fopen(path, "wb");
   if (!f)
      return -1;
   if (len > 0 && data && fwrite(data, 1, len, f) != len)
   {
      fclose(f);
      return -1;
   }
   if (fclose(f) != 0)
      return -1;
   return 0;
}

static int shared_stat(const workspace_provider_t *p, const char *path, ws_stat_t *st)
{
   (void)p;
   if (!st)
      return -1;
   st->exists = 0;
   st->is_dir = 0;
   st->size = 0;

   struct _stat sb;
   if (path && _stat(path, &sb) == 0)
   {
      st->exists = 1;
      st->is_dir = (sb.st_mode & _S_IFDIR) ? 1 : 0;
      st->size = (sb.st_mode & _S_IFDIR) ? 0 : (long)sb.st_size;
   }
   return 0;
}

/* List dir entries matching pattern (default *), returning full "dir/name"
 * paths to mirror the POSIX glob provider. Uses the Win32 CRT _findfirst API. */
static int shared_list(const workspace_provider_t *p, const char *dir, const char *pattern,
                       char ***out, int *count)
{
   (void)p;
   if (out)
      *out = NULL;
   if (count)
      *count = 0;
   if (!dir || !out || !count)
      return -1;

   char find_pat[4096];
   snprintf(find_pat, sizeof(find_pat), "%s/%s", dir, (pattern && pattern[0]) ? pattern : "*");

   struct _finddata_t fd;
   intptr_t h = _findfirst(find_pat, &fd);
   if (h == -1)
   {
      /* No match (or unreadable dir) is an empty list, like GLOB_NOMATCH. */
      *out = NULL;
      *count = 0;
      return 0;
   }

   char **arr = NULL;
   int n = 0, cap = 0;
   do
   {
      if (strcmp(fd.name, ".") == 0 || strcmp(fd.name, "..") == 0)
         continue;
      if (n == cap)
      {
         int ncap = cap ? cap * 2 : 16;
         char **na = realloc(arr, (size_t)ncap * sizeof(char *));
         if (!na)
         {
            ws_provider_free_list(arr, n);
            _findclose(h);
            return -1;
         }
         arr = na;
         cap = ncap;
      }
      char full[4096];
      snprintf(full, sizeof(full), "%s/%s", dir, fd.name);
      arr[n] = _strdup(full);
      if (!arr[n])
      {
         ws_provider_free_list(arr, n);
         _findclose(h);
         return -1;
      }
      n++;
   } while (_findnext(h, &fd) == 0);
   _findclose(h);

   *out = arr;
   *count = n;
   return 0;
}

/* Append a quoted argv token to a command-line buffer (CRT quoting is enough
 * for the cmd.exe pipeline we spawn). Returns bytes written, 0 on overflow. */
static size_t append_quoted(char *buf, size_t cap, size_t off, const char *tok)
{
   int w = snprintf(buf + off, cap - off, "%s\"%s\"", off ? " " : "", tok ? tok : "");
   if (w < 0 || (size_t)w >= cap - off)
      return 0;
   return (size_t)w;
}

/* Run `cmdline` through cmd.exe, capturing combined output, honouring the
 * runner's thread-local cwd by prefixing `cd /d "<cwd>" &&`. Caller frees. */
static char *win_capture(const char *cmdline, int *exit_code)
{
   if (exit_code)
      *exit_code = -1;
   if (!cmdline)
      return NULL;

   /* Combine stderr into stdout (the provider contract) and honour the runner's
    * thread-local cwd by prefixing a cmd.exe `cd /d`. */
   const char *cwd = run_cmd_get_cwd();
   size_t need = strlen(cmdline) + (cwd ? strlen(cwd) : 0) + 48;
   char *full = malloc(need);
   if (!full)
      return NULL;
   if (cwd && cwd[0])
      snprintf(full, need, "cd /d \"%s\" && %s 2>&1", cwd, cmdline);
   else
      snprintf(full, need, "%s 2>&1", cmdline);

   FILE *f = _popen(full, "r");
   free(full);
   if (!f)
      return NULL;

   size_t cap = 65536, len = 0;
   char *buf = malloc(cap);
   if (!buf)
   {
      _pclose(f);
      return NULL;
   }
   size_t r;
   while ((r = fread(buf + len, 1, cap - len - 1, f)) > 0)
   {
      len += r;
      if (len + 1 >= cap)
      {
         size_t ncap = cap * 2;
         char *nb = realloc(buf, ncap);
         if (!nb)
            break;
         buf = nb;
         cap = ncap;
      }
   }
   buf[len] = '\0';
   int rc = _pclose(f);
   if (exit_code)
      *exit_code = rc;
   return buf;
}

static int shared_exec(const workspace_provider_t *p, const char *const argv[], char **out,
                       size_t max_out)
{
   (void)p;
   if (out)
      *out = NULL;
   if (!argv || !argv[0])
      return -1;

   char cmdline[8192];
   size_t off = 0;
   for (int i = 0; argv[i]; i++)
   {
      size_t w = append_quoted(cmdline, sizeof(cmdline), off, argv[i]);
      if (!w)
         return -1;
      off += w;
   }

   int rc = -1;
   char *captured = win_capture(cmdline, &rc);
   if (out && captured)
   {
      if (max_out && strlen(captured) >= max_out)
         captured[max_out - 1] = '\0';
      *out = captured;
   }
   else
   {
      free(captured);
   }
   return rc;
}

static char *shared_exec_shell(const workspace_provider_t *p, const char *cmd, int *exit_code)
{
   (void)p;
   return win_capture(cmd, exit_code);
}

static const workspace_provider_t g_shared_provider = {
    .kind = WS_PROVIDER_SHARED,
    .read_all = shared_read_all,
    .write_all = shared_write_all,
    .stat = shared_stat,
    .list = shared_list,
    .exec = shared_exec,
    .exec_shell = shared_exec_shell,
};

const workspace_provider_t *workspace_provider_shared(void)
{
   return &g_shared_provider;
}

/* Mirrors posix/workspace_provider.c: free each entry, then the array. NULL-safe. */
void ws_provider_free_list(char **entries, int count)
{
   if (!entries)
      return;
   for (int i = 0; i < count; i++)
      free(entries[i]);
   free(entries);
}
