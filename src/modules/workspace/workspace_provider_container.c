/* workspace_provider_container.c: the workspace provider that runs a delegate's
 * file/exec ops inside its container, via a delegate backend. See the header. */
#include "workspace_provider_container.h"

#include "aimee.h"
#include "dstr.h"
#include "log.h"
#include <fnmatch.h>
#include <stdint.h> /* uintptr_t */
#include <stdlib.h>
#include <string.h>

/* Cap for a captured exec: the backend writes into caller-provided buffers, and
 * the provider contract lets a caller ask for less via max_out. */
#define WSC_EXEC_CAP 65536

static ws_container_provider_t *wsc_self(const workspace_provider_t *p)
{
   /* base is first in the struct, so the cast is the documented pattern (mirrors
    * ws_detached_provider_t). const is dropped because the backend vtable takes a
    * non-const self; the provider itself is not mutated. */
   return (ws_container_provider_t *)(void *)(uintptr_t)(const void *)p;
}

/* Wrap `raw` in single quotes for /bin/sh, escaping embedded single quotes as
 * '\'' — the only shell metacharacter that survives single quoting. Returns a
 * malloc'd string (caller frees), or NULL on OOM.
 *
 * exec(argv) must become one command line for the backend, which takes a string.
 * Joining with spaces and no quoting would let an argument containing a space,
 * a quote or a `;` become a second command — the caller passed argv precisely to
 * avoid a shell, so re-introducing one unquoted would be worse than not having
 * the seam. */
static char *wsc_shell_quote(const char *raw)
{
   if (!raw)
      return NULL;
   dstr_t q;
   dstr_init(&q);
   dstr_append_char(&q, '\'');
   for (const char *c = raw; *c; c++)
   {
      if (*c == '\'')
         dstr_append_str(&q, "'\\''");
      else
         dstr_append_char(&q, *c);
   }
   dstr_append_char(&q, '\'');
   char *out = q.data ? safe_strdup(q.data) : NULL;
   dstr_free(&q);
   return out;
}

/* Run `cmd` in the container, capturing combined stdout+stderr. Returns the
 * command's exit code, or -1 if the backend could not run it at all. `*out` (when
 * non-NULL) receives a malloc'd NUL-terminated capture; caller frees. */
static int wsc_run(const ws_container_provider_t *self, const char *cmd, char **out, size_t max_out)
{
   if (out)
      *out = NULL;
   if (!self || !self->backend || !self->backend->exec || !cmd)
      return -1;

   size_t cap = (max_out && max_out < WSC_EXEC_CAP) ? max_out : WSC_EXEC_CAP;
   char *sbuf = calloc(1, cap + 1);
   char *ebuf = calloc(1, cap + 1);
   if (!sbuf || !ebuf)
   {
      free(sbuf);
      free(ebuf);
      return -1;
   }

   delegate_exec_result_t r = {.exit_code = -1,
                               .latency_ms = 0,
                               .stdout_buf = sbuf,
                               .stdout_cap = cap + 1,
                               .stderr_buf = ebuf,
                               .stderr_cap = cap + 1};
   ws_container_provider_t *mut = wsc_self(&self->base);
   int rc = mut->backend->exec(mut->backend, mut->state, cmd, 0, &r);
   if (rc != 0)
   {
      free(sbuf);
      free(ebuf);
      return -1; /* transport failure, distinct from a non-zero exit code */
   }

   if (out)
   {
      /* The contract says "combined stdout+stderr". Keep stderr: a delegate whose
       * build fails needs the compiler's message, and dropping it would turn a
       * diagnosable failure into a bare exit code. */
      dstr_t joined;
      dstr_init(&joined);
      if (sbuf[0])
         dstr_append_str(&joined, sbuf);
      if (ebuf[0])
      {
         if (joined.len)
            dstr_append_char(&joined, '\n');
         dstr_append_str(&joined, ebuf);
      }
      if (joined.len && joined.data)
         *out = safe_strdup(joined.data);
      dstr_free(&joined);
   }
   free(sbuf);
   free(ebuf);
   return r.exit_code;
}

static int wsc_read_all(const workspace_provider_t *p, const char *path, char **out, size_t *len)
{
   if (out)
      *out = NULL;
   if (len)
      *len = 0;
   ws_container_provider_t *self = wsc_self(p);
   if (!self->backend || !self->backend->read_file || !path || !out)
      return -1;
   char *buf = NULL;
   /* limit 0 = to EOF (the backend caps it at 16 MiB to bound the malloc). */
   if (self->backend->read_file(self->backend, self->state, path, 0, 0, &buf) != 0 || !buf)
      return -1;
   *out = buf;
   if (len)
      *len = strlen(buf);
   return 0;
}

static int wsc_write_all(const workspace_provider_t *p, const char *path, const char *data,
                         size_t len)
{
   ws_container_provider_t *self = wsc_self(p);
   if (!self->backend || !self->backend->write_file || !path)
      return -1;

   /* The backend's write_file takes a NUL-terminated string and strlen()s it, so
    * content with an embedded NUL would be silently truncated at the first one.
    * Refuse instead: a delegate that writes a binary file and is told it succeeded
    * would corrupt it and find out much later. Loud beats plausible.
    *
    * Lifting this means giving the backend a length-aware write; the b64 wire it
    * already uses would carry it fine. Tracked with the sandbox work. */
   if (data && memchr(data, '\0', len) != NULL)
   {
      LOG_ERROR("ws-container",
                "refusing binary write to '%s' (%zu bytes, embedded NUL): the container backend's "
                "write_file is NUL-terminated and would truncate it silently",
                path, len);
      return -1;
   }

   /* write_all(data=NULL, len=0) writes an empty file, per the contract. */
   char *tmp = NULL;
   if (len)
   {
      tmp = malloc(len + 1);
      if (!tmp)
         return -1;
      memcpy(tmp, data, len);
      tmp[len] = '\0';
   }
   int rc = self->backend->write_file(self->backend, self->state, path, tmp ? tmp : "");
   free(tmp);
   return rc == 0 ? 0 : -1;
}

static int wsc_stat(const workspace_provider_t *p, const char *path, ws_stat_t *st)
{
   if (st)
      memset(st, 0, sizeof(*st));
   ws_container_provider_t *self = wsc_self(p);
   if (!st || !path)
      return 0; /* contract: always returns 0; exists stays 0 */

   char *q = wsc_shell_quote(path);
   if (!q)
      return 0;
   /* One round trip: kind and size together. `-` for a size we could not take
    * (a directory, or a stat that failed) keeps the parse total. */
   char cmd[MAX_PATH_LEN + 128];
   snprintf(cmd, sizeof(cmd),
            "if [ -d %s ]; then echo 'd -'; elif [ -e %s ]; then echo \"f $(wc -c < %s 2>/dev/null "
            "|| echo -)\"; else echo 'x -'; fi",
            q, q, q);
   free(q);

   char *out = NULL;
   if (wsc_run(self, cmd, &out, 256) < 0 || !out)
   {
      free(out);
      return 0;
   }
   if (out[0] == 'd')
   {
      st->exists = 1;
      st->is_dir = 1;
   }
   else if (out[0] == 'f')
   {
      st->exists = 1;
      st->is_dir = 0;
      long sz = 0;
      if (sscanf(out + 1, " %ld", &sz) == 1 && sz >= 0)
         st->size = sz;
   }
   free(out);
   return 0;
}

static int wsc_list(const workspace_provider_t *p, const char *dir, const char *pattern,
                    char ***out, int *count)
{
   if (out)
      *out = NULL;
   if (count)
      *count = 0;
   ws_container_provider_t *self = wsc_self(p);
   if (!self->backend || !self->backend->list_dir || !dir || !out || !count)
      return -1;

   char **entries = NULL;
   if (self->backend->list_dir(self->backend, self->state, dir, &entries) != 0)
      return -1;
   if (!entries)
   {
      *count = 0;
      return 0; /* zero matches is success, per the contract */
   }

   /* The backend lists a directory; the contract wants dir/pattern globbed. Filter
    * here rather than shelling out again: the backend's list is already anchored in
    * the workspace, and a second exec would re-open the path-resolution question the
    * backend just answered. */
   int n = 0;
   while (entries[n])
      n++;
   char **keep = calloc((size_t)n + 1, sizeof(*keep));
   if (!keep)
   {
      for (int i = 0; i < n; i++)
         free(entries[i]);
      free(entries);
      return -1;
   }
   int kept = 0;
   for (int i = 0; i < n; i++)
   {
      const char *base = strrchr(entries[i], '/');
      base = base ? base + 1 : entries[i];
      if (!pattern || !pattern[0] || strcmp(pattern, "*") == 0 || fnmatch(pattern, base, 0) == 0)
         keep[kept++] = entries[i];
      else
         free(entries[i]);
   }
   free(entries);
   *out = keep;
   *count = kept;
   return 0;
}

static int wsc_exec(const workspace_provider_t *p, const char *const argv[], char **out,
                    size_t max_out)
{
   if (out)
      *out = NULL;
   ws_container_provider_t *self = wsc_self(p);
   if (!argv || !argv[0])
      return -1;

   /* argv -> one quoted command line for the backend's string-taking exec. */
   dstr_t cmd;
   dstr_init(&cmd);
   for (int i = 0; argv[i]; i++)
   {
      char *q = wsc_shell_quote(argv[i]);
      if (!q)
      {
         dstr_free(&cmd);
         return -1;
      }
      if (cmd.len)
         dstr_append_char(&cmd, ' ');
      dstr_append_str(&cmd, q);
      free(q);
   }
   int rc = cmd.data ? wsc_run(self, cmd.data, out, max_out) : -1;
   dstr_free(&cmd);
   return rc;
}

static char *wsc_exec_shell(const workspace_provider_t *p, const char *cmd, int *exit_code)
{
   ws_container_provider_t *self = wsc_self(p);
   char *out = NULL;
   int rc = wsc_run(self, cmd, &out, WSC_EXEC_CAP);
   if (exit_code)
      *exit_code = rc; /* -1 on spawn/transport failure, per the contract */
   return out;
}

int ws_container_provider_init(ws_container_provider_t *out, delegate_backend_t *backend,
                               void *state)
{
   /* Refuse a half-bound provider: every op would silently fall through to the
    * host, which is the precise failure the sandbox exists to prevent. */
   if (!out || !backend || !state)
      return -1;
   memset(out, 0, sizeof(*out));
   out->base.kind = WS_PROVIDER_CONTAINER;
   out->base.read_all = wsc_read_all;
   out->base.write_all = wsc_write_all;
   out->base.stat = wsc_stat;
   out->base.list = wsc_list;
   out->base.exec = wsc_exec;
   out->base.exec_shell = wsc_exec_shell;
   /* exec_stream stays NULL: the backend has no streaming op, and the contract
    * permits NULL. A provider-CLI delegate (`claude -p`) therefore cannot yet run
    * through this provider — it needs streaming to surface a turn. Tracked. */
   out->base.exec_stream = NULL;
   out->backend = backend;
   out->state = state;
   return 0;
}
