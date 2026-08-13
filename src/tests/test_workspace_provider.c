/* test_workspace_provider.c: the `shared` resource provider must be a faithful,
 * byte-for-byte stand-in for direct filesystem access (the workspace-resource-
 * plane Phase-1 parity contract). */
#include "modules/workspace/workspace_provider.h"

#include <assert.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* run_cmd cwd control (util.c): exec_shell honors this thread-local, mirroring
 * how the mcp_git tools set their working directory before shelling out. */
extern void run_cmd_set_cwd(const char *cwd);

static char g_tmp[256];

static void make_tmpdir(void)
{
   snprintf(g_tmp, sizeof(g_tmp), "%s/ws_provider_test.XXXXXX", platform_tmpdir());
   char *d = mkdtemp(g_tmp);
   assert(d != NULL);
}

static char *join(const char *name)
{
   static char path[512];
   snprintf(path, sizeof(path), "%s/%s", g_tmp, name);
   return path;
}

/* Read a file with plain libc, the "ground truth" the provider must match. */
static char *direct_read(const char *path, size_t *len)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   char *buf = malloc((size_t)sz + 1);
   size_t rd = fread(buf, 1, (size_t)sz, f);
   fclose(f);
   buf[rd] = '\0';
   if (len)
      *len = rd;
   return buf;
}

int main(void)
{
   make_tmpdir();
   const workspace_provider_t *ws = workspace_provider_shared();
   assert(ws != NULL);
   assert(ws->kind == WS_PROVIDER_SHARED);

   /* --- kind helpers --- */
   assert(ws_provider_kind_from_string("shared") == WS_PROVIDER_SHARED);
   assert(ws_provider_kind_from_string("detached") == WS_PROVIDER_DETACHED);
   assert(ws_provider_kind_from_string(NULL) == WS_PROVIDER_SHARED);
   assert(ws_provider_kind_from_string("bogus") == WS_PROVIDER_SHARED);
   assert(strcmp(ws_provider_kind_to_string(WS_PROVIDER_SHARED), "shared") == 0);
   assert(strcmp(ws_provider_kind_to_string(WS_PROVIDER_DETACHED), "detached") == 0);
   /* detached falls back to shared for now, but never NULL */
   assert(workspace_provider_for_kind(WS_PROVIDER_DETACHED) != NULL);

   /* --- active provider: thread-local selection seam, defaults to shared --- */
   {
      assert(workspace_provider_active() == ws); /* default == shared */
      workspace_provider_t fake;
      memset(&fake, 0, sizeof(fake));
      fake.kind = WS_PROVIDER_DETACHED;
      workspace_provider_set_active(&fake);
      assert(workspace_provider_active() == &fake);
      workspace_provider_clear_active();
      assert(workspace_provider_active() == ws); /* back to shared */
   }

   /* --- write_all + read_all round-trip, byte-for-byte vs direct fs --- */
   {
      const char *path = join("hello.txt");
      const char *content = "line1\nline2\nembedded\0nul\ntail\n"; /* contains a NUL */
      size_t clen = 30;                                            /* includes the NUL byte */
      assert(ws->write_all(ws, path, content, clen) == 0);

      char *got = NULL;
      size_t glen = 0;
      assert(ws->read_all(ws, path, &got, &glen) == 0);
      assert(glen == clen);
      assert(memcmp(got, content, clen) == 0);
      assert(got[glen] == '\0'); /* NUL-terminated one past len */

      size_t dlen = 0;
      char *direct = direct_read(path, &dlen);
      assert(direct && dlen == glen && memcmp(direct, got, glen) == 0);
      free(direct);
      free(got);
   }

   /* --- write_all empty file (NULL data, len 0) --- */
   {
      const char *path = join("empty.txt");
      assert(ws->write_all(ws, path, NULL, 0) == 0);
      ws_stat_t st;
      assert(ws->stat(ws, path, &st) == 0);
      assert(st.exists == 1 && st.is_dir == 0 && st.size == 0);
      char *got = NULL;
      size_t glen = 1;
      assert(ws->read_all(ws, path, &got, &glen) == 0);
      assert(glen == 0 && got != NULL && got[0] == '\0');
      free(got);
   }

   /* --- read_all / stat on a missing path --- */
   {
      const char *path = join("does_not_exist");
      char *got = (char *)0x1;
      assert(ws->read_all(ws, path, &got, NULL) == -1);
      assert(got == NULL);
      ws_stat_t st;
      assert(ws->stat(ws, path, &st) == 0);
      assert(st.exists == 0);
   }

   /* --- stat on a directory --- */
   {
      ws_stat_t st;
      assert(ws->stat(ws, g_tmp, &st) == 0);
      assert(st.exists == 1 && st.is_dir == 1 && st.size == 0);
   }

   /* --- list: matches direct glob, in glob order --- */
   {
      assert(ws->write_all(ws, join("a.c"), "x", 1) == 0);
      assert(ws->write_all(ws, join("b.c"), "y", 1) == 0);
      assert(ws->write_all(ws, join("c.txt"), "z", 1) == 0);

      char **entries = NULL;
      int n = 0;
      assert(ws->list(ws, g_tmp, "*.c", &entries, &n) == 0);

      char pat[512];
      snprintf(pat, sizeof(pat), "%s/*.c", g_tmp);
      glob_t gg;
      memset(&gg, 0, sizeof(gg));
      int rc = glob(pat, GLOB_NOSORT, NULL, &gg);
      assert(rc == 0);
      assert((int)gg.gl_pathc == n);
      for (int i = 0; i < n; i++)
         assert(strcmp(entries[i], gg.gl_pathv[i]) == 0);
      globfree(&gg);
      ws_provider_free_list(entries, n);
   }

   /* --- list: no match is success with count 0 (the old GLOB_NOMATCH) --- */
   {
      char **entries = (char **)0x1;
      int n = -1;
      assert(ws->list(ws, g_tmp, "*.nope", &entries, &n) == 0);
      assert(n == 0 && entries == NULL);
   }

   /* --- list: default pattern ("") lists everything under the dir --- */
   {
      char **entries = NULL;
      int n = 0;
      assert(ws->list(ws, g_tmp, NULL, &entries, &n) == 0);
      assert(n >= 4); /* hello.txt, empty.txt, a.c, b.c, c.txt */
      ws_provider_free_list(entries, n);
   }

   /* --- exec: shared runs the command locally and captures output --- */
   {
      const char *true_argv[] = {"true", NULL};
      char *out = NULL;
      assert(ws->exec(ws, true_argv, &out, 4096) == 0);
      free(out);

      const char *false_argv[] = {"false", NULL};
      out = NULL;
      assert(ws->exec(ws, false_argv, &out, 4096) != 0);
      free(out);

      const char *echo_argv[] = {"echo", "ws-exec-ok", NULL};
      out = NULL;
      assert(ws->exec(ws, echo_argv, &out, 4096) == 0);
      assert(out && strstr(out, "ws-exec-ok") != NULL);
      free(out);
   }

   /* --- exec_shell: shared runs a /bin/sh command-line, honoring shell
    * features (redirection) and the thread-local run_cmd cwd. The seam the
    * legacy mcp_git shell-string call sites route through. --- */
   {
      int rc = -1;
      /* stdout is captured */
      char *out = ws->exec_shell(ws, "echo ws-shell-ok", &rc);
      assert(rc == 0);
      assert(out && strstr(out, "ws-shell-ok") != NULL);
      free(out);

      /* combined capture: a stderr writer with 2>&1 lands in the buffer
       * (the redirection the legacy mcp_git commands rely on) */
      rc = 0;
      out = ws->exec_shell(ws, "ls /no_such_ws_path_xyz 2>&1", &rc);
      assert(rc != 0);
      assert(out && strstr(out, "no_such_ws_path_xyz") != NULL);
      free(out);

      /* non-zero exit status is surfaced */
      rc = 0;
      out = ws->exec_shell(ws, "exit 3", &rc);
      assert(rc != 0);
      free(out);

      /* thread-local run_cmd cwd is honored (mcp_git sets it via
       * run_cmd_set_cwd before shelling out) */
      run_cmd_set_cwd(g_tmp);
      rc = -1;
      out = ws->exec_shell(ws, "pwd", &rc);
      run_cmd_set_cwd(NULL);
      assert(rc == 0);
      /* match on the unique mkdtemp basename token (robust to a /tmp symlink) */
      assert(out && strstr(out, "ws_provider_test.") != NULL);
      free(out);
   }

   /* cleanup */
   ws->write_all(ws, join("hello.txt"), "", 0);
   {
      char **entries = NULL;
      int n = 0;
      if (ws->list(ws, g_tmp, NULL, &entries, &n) == 0)
      {
         for (int i = 0; i < n; i++)
            unlink(entries[i]);
         ws_provider_free_list(entries, n);
      }
   }
   rmdir(g_tmp);

   printf("workspace_provider: all tests passed\n");
   return 0;
}
