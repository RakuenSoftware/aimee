/* kb_curator_sidecar.c: shared LLM-sidecar invocation. See header. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_curator_sidecar.h"
#include "cJSON.h" /* the sidecar's own error payload is JSON */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CS_DEFAULT_OUTBUF 8192

/* Hard wall-clock cap for the sidecar, mirroring the code-unit stage's bound.
 *
 * This is load-bearing for the stale-lease reclaims (kb_curator_extract.c,
 * kb_memory_facts.c), not just hygiene. An unbounded popen/pclose wedges its
 * drain thread forever — and a reclaim that re-arms a job whose original worker
 * is STILL running would hand the same job to a second worker: duplicate
 * artifacts, and the wedged thread later clobbering the status of a job it no
 * longer owns. A lease is only safe if the work under it is bounded well below
 * the lease. 300s sits comfortably under the 15-minute lease (and far above the
 * 13-22s a real doc extraction takes), so a job that outlives this cap is
 * genuinely wedged and its slot is free by the time the reclaim fires. */
#define CS_SIDECAR_TIMEOUT_S 300

/* Quote `in` as a single sh word. See the header for why this is required.
 *
 * The POSIX-portable form: wrap in single quotes, and render each embedded
 * single quote as '\'' (close, escaped quote, reopen). Nothing inside single
 * quotes is special to the shell, so this is total — no per-metacharacter
 * escaping to get subtly wrong. */
int kb_curator_shell_quote(const char *in, char *out, size_t outlen)
{
   if (!in || !out || outlen < 3)
      return -1;

   size_t o = 0;
   out[o++] = '\'';
   for (const char *p = in; *p; p++)
   {
      if (*p == '\'')
      {
         /* '\'' — 4 bytes, plus the closing quote and NUL still to come. */
         if (o + 4 + 2 > outlen)
            return -1;
         out[o++] = '\'';
         out[o++] = '\\';
         out[o++] = '\'';
         out[o++] = '\'';
      }
      else
      {
         if (o + 1 + 2 > outlen)
            return -1;
         out[o++] = *p;
      }
   }
   out[o++] = '\'';
   out[o] = '\0';
   return 0;
}

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
         /* 124 is timeout(1)'s deadline signal, but it also propagates a wrapped
          * command's own exit code — so a sidecar that exits 124 itself is
          * indistinguishable from here. Same honesty as the 128+n case below:
          * name the likely cause without asserting it as fact. */
         snprintf(errbuf, errlen,
                  "sidecar exited 124 (timeout after %ds, or the command itself "
                  "exited 124)",
                  timeout_s);
      else if (code > 128)
         /* A shell reports a signal-killed child as 128+n, so 137 is very likely
          * an OOM kill — but a process can also exit(137) of its own accord, and
          * the two are indistinguishable from here. Report the exit code as the
          * fact and the signal as the reading, rather than asserting a kill that
          * may not have happened and sending an operator hunting a phantom OOM. */
         snprintf(errbuf, errlen, "sidecar exited %d (128+%d: likely killed by signal %d, %s)",
                  code, code - 128, code - 128, strsignal(code - 128));
      else
         snprintf(errbuf, errlen, "sidecar exited %d", code);
      return;
   }
   snprintf(errbuf, errlen, "sidecar ended abnormally (wait status %d)", status);
}

/* Append the sidecar's own error to errbuf. See the header for why.
 *
 * Parses rather than string-matches: the payload is JSON the sidecar wrote, and
 * cJSON is already linked everywhere this file is. Anything that is not a
 * well-formed {"status":"error","error":<string>} is left alone — a non-zero exit
 * with garbage on stdout should keep the wait-status description, not inherit a
 * misleading fragment of it. */
void kb_curator_append_sidecar_error(const char *out, char *errbuf, size_t errlen)
{
   if (!out || !out[0] || !errbuf || errlen == 0)
      return;

   cJSON *root = cJSON_Parse(out);
   if (!root)
      return;
   const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
   const cJSON *why = cJSON_GetObjectItemCaseSensitive(root, "error");
   if (cJSON_IsString(status) && strcmp(status->valuestring, "error") == 0 && cJSON_IsString(why) &&
       why->valuestring[0])
   {
      size_t used = strlen(errbuf);
      if (used + 2 < errlen)
         snprintf(errbuf + used, errlen - used, ": %s", why->valuestring);
   }
   cJSON_Delete(root);
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

   /* Bound the sidecar with coreutils `timeout` (present on every image — they
    * are all debian-slim): SIGTERM at the ceiling, SIGKILL 10s later if it
    * ignores TERM. Run the configured command under `sh -c` INSIDE timeout so it
    * keeps the shell semantics popen() gave it (env-var prefixes like `FOO=bar
    * python …`, builtins, operators) — passing it as timeout's own argv would
    * break those and let compound commands escape the bound.
    *
    * The invariant is that adding the timeout wrapper changes NOTHING about how
    * the command is interpreted, which takes two things:
    *
    *  - QUOTE the script. A second shell parses this line, so a command
    *    containing a quote, $ or backtick would otherwise be re-parsed (and
    *    $VARs expanded twice) — commands that worked under the bare popen.
    *
    *  - Keep the redirection INSIDE the command's own parse context. The old
    *    form was `sh -c "<cmd> < tmp"`, where `< tmp` is parsed alongside cmd:
    *    for `a | b` it binds to b, the last pipeline command. Redirecting the
    *    wrapper's stdin instead would hand the file to `a` — a silent change in
    *    meaning. So the script keeps the same `<cmd> < <path>` shape the bare
    *    popen produced, with the path shell-quoted as a LITERAL.
    *
    * The path is embedded literally rather than passed as a positional ($1):
    * with `sh -c '<cmd> < "$1"' sh <path>`, a configured command containing
    * `set --` would rebind $1 before the redirection expanded and read a
    * different file. A literal cannot be reassigned. */
   char qtmp[1040]; /* worst case: every byte of a 256-char path is a quote */
   if (kb_curator_shell_quote(tmppath, qtmp, sizeof(qtmp)) != 0)
   {
      unlink(tmppath);
      if (errbuf)
         snprintf(errbuf, errlen, "sidecar temp path too long to quote safely");
      return NULL;
   }

   char script[1600];
   int sn = snprintf(script, sizeof(script), "%s < %s", cmd, qtmp);
   char qscript[6464]; /* worst case: every byte of the script is a quote */
   if (sn < 0 || (size_t)sn >= sizeof(script) ||
       kb_curator_shell_quote(script, qscript, sizeof(qscript)) != 0)
   {
      unlink(tmppath);
      if (errbuf)
         snprintf(errbuf, errlen, "sidecar command too long to quote safely");
      return NULL;
   }

   char full_cmd[6528];
   snprintf(full_cmd, sizeof(full_cmd), "timeout -k 10 %d sh -c %s", CS_SIDECAR_TIMEOUT_S, qscript);

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
      kb_curator_describe_wait_status(rc, CS_SIDECAR_TIMEOUT_S, errbuf, errlen);
      /* Before discarding the output: the sidecar may have explained itself in
       * it. Keep the reason, not just the exit code. */
      kb_curator_append_sidecar_error(out, errbuf, errlen);
      free(out);
      return NULL;
   }
   return out;
}
