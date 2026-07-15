/* kb_curator_sidecar.c: shared LLM-sidecar invocation. See header. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_curator_sidecar.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CS_DEFAULT_OUTBUF 8192

/* Render a pclose(3) return value into an operator-legible reason.
 *
 * pclose returns a wait(2)-encoded status, NOT an exit code: reporting it raw
 * printed "sidecar exited 256" for a plain `exit(1)` (1 << 8), which reads like
 * an exotic failure and sends you looking for a signal that never happened.
 *
 * timeout_s > 0 means the caller wrapped the command in coreutils timeout(1),
 * which reports the wall-clock cap as exit 124 — only then can 124 be read as a
 * timeout rather than the command's own exit code. Callers that do not wrap
 * pass 0. */
void kb_curator_describe_wait_status(int status, int timeout_s, char *errbuf, size_t errlen)
{
   if (!errbuf || errlen == 0)
      return;

   if (status == -1)
   {
      snprintf(errbuf, errlen, "pclose failed: %s", strerror(errno));
      return;
   }
   if (WIFSIGNALED(status))
   {
      int sig = WTERMSIG(status);
      snprintf(errbuf, errlen, "sidecar killed by signal %d (%s)", sig, strsignal(sig));
      return;
   }
   if (WIFEXITED(status))
   {
      int code = WEXITSTATUS(status);
      if (timeout_s > 0 && code == 124)
         snprintf(errbuf, errlen, "sidecar timed out after %ds", timeout_s);
      else if (code > 128)
         /* A shell reports a signal-killed child as 128+n; distinguishing which
          * signal is what tells OOM (9) apart from a clean TERM (15). */
         snprintf(errbuf, errlen, "sidecar killed by signal %d (%s)", code - 128,
                  strsignal(code - 128));
      else
         snprintf(errbuf, errlen, "sidecar exited %d", code);
      return;
   }
   snprintf(errbuf, errlen, "sidecar ended abnormally (wait status %d)", status);
}

char *kb_curator_sidecar_run(const char *cmd, const char *json_input, int out_cap, char *errbuf,
                             size_t errlen)
{
   if (errbuf && errlen)
      errbuf[0] = '\0';
   if (!cmd || !cmd[0])
   {
      if (errbuf)
         snprintf(errbuf, errlen, "no command configured");
      return NULL;
   }
   size_t cap = out_cap > 0 ? (size_t)out_cap : CS_DEFAULT_OUTBUF;

   /* Write the request JSON to a temp file, then pipe it into the command.
    * Honour TMPDIR (as code_collect.c does) so the spool can be moved off a full
    * or slow filesystem, and carry errno on failure — ENOSPC, EMFILE and EACCES
    * are different incidents, and this string is the only forensic record. */
   const char *tmpdir = getenv("TMPDIR");
   if (!tmpdir || !tmpdir[0])
      tmpdir = "/tmp";
   char tmppath[256];
   snprintf(tmppath, sizeof(tmppath), "%s/aimee_curator_sidecar_XXXXXX", tmpdir);
   int fd = mkstemp(tmppath);
   if (fd < 0)
   {
      if (errbuf)
         snprintf(errbuf, errlen, "mkstemp failed for %s: %s", tmppath, strerror(errno));
      return NULL;
   }

   size_t inlen = json_input ? strlen(json_input) : 0;
   if (inlen && write(fd, json_input, inlen) != (ssize_t)inlen)
   {
      close(fd);
      unlink(tmppath);
      if (errbuf)
         snprintf(errbuf, errlen, "write to tmpfile failed");
      return NULL;
   }
   close(fd);

   char full_cmd[1024];
   snprintf(full_cmd, sizeof(full_cmd), "%s < %s", cmd, tmppath);

   FILE *fp = popen(full_cmd, "r");
   if (!fp)
   {
      unlink(tmppath);
      if (errbuf)
         snprintf(errbuf, errlen, "popen failed for: %s", full_cmd);
      return NULL;
   }

   char *out = malloc(cap);
   if (!out)
   {
      pclose(fp);
      unlink(tmppath);
      if (errbuf)
         snprintf(errbuf, errlen, "out of memory");
      return NULL;
   }

   size_t total = 0;
   size_t n;
   while ((n = fread(out + total, 1, cap - total - 1, fp)) > 0)
   {
      total += n;
      if (total >= cap - 1)
         break;
   }
   out[total] = '\0';
   int rc = pclose(fp);
   unlink(tmppath);

   if (rc != 0)
   {
      /* This caller does not wrap the command in timeout(1), so 124 carries no
       * special meaning here. */
      kb_curator_describe_wait_status(rc, 0, errbuf, errlen);
      free(out);
      return NULL;
   }
   return out;
}
