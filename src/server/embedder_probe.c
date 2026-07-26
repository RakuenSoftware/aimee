/* embedder_probe.c -- §2b kb-side embedder /health dim probe. Registered with the
 * db2 layer so db2_init can derive a fresh DB's embedding dim from the running
 * embedder without db2 learning the embed transport (db2 stays config-free). */
#include "embedder_probe.h"
#include "platform_process.h"

#include "aimee.h" /* EMBED_MAX_DIM + memory.h prerequisites */
#include "lifecycle.h"
#include "log.h"
#include "memory.h"               /* memory_embed_text — in-process HTTP probe */
#include "memory_core_internal.h" /* memory_embed_command_is_http */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The configured embed command (e.g. "python3 /opt/aimee/scripts/embed-remote.py"),
 * captured at registration. Operator-trusted config, run via /bin/sh with --dim. */
static char g_embed_cmd[1024] = "";

/* Per-attempt bounds. The floor keeps a nearly-exhausted budget from degenerating
 * into a zero-timeout attempt; the ceiling keeps one attempt from consuming a
 * large budget entirely and starving the two-consecutive-reads check. */
#define PROBE_ATTEMPT_MIN_MS 2000
#define PROBE_ATTEMPT_MAX_MS 15000

static long probe_mono_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* One probe attempt: run `<embed_cmd> --dim`, return the parsed positive dim, or
 * <=0 if the embedder is not ready / the command failed / output was not a single
 * positive integer. */
static int probe_once(int timeout_ms)
{
   if (!g_embed_cmd[0])
      return -1;
   /* An http(s):// "command" is the in-process embed transport (the combined /
    * unified-container deployments export AIMEE_LLM_URL and set no sidecar
    * command) — there is nothing to exec, and popen would fork
    * `sh -c "http://... --dim"` forever. Probe by embedding a short text and
    * taking the vector's length: transport-exact, and independent of whether
    * the gateway's /health reports a dim. */
   if (memory_embed_command_is_http(g_embed_cmd))
   {
      static float vec[EMBED_MAX_DIM];
      int d = memory_embed_text("dim probe", g_embed_cmd, vec, EMBED_MAX_DIM);
      return d > 0 ? d : -1;
   }
   char cmd[1100];
   snprintf(cmd, sizeof(cmd), "%s --dim", g_embed_cmd);

   /* Bounded, because this runs on the MAIN THREAD during kb startup, before the
    * HTTP listener exists. The previous popen()/fread() had no timeout, so an
    * embedder that accepted the exec and then never answered blocked startup
    * forever — the process stayed alive with no listener, which reads as "up but
    * not serving". Observed directly: thread 1 parked in fread() under
    * __libc_start_main with the listener never created.
    *
    * That also made embedder_probe_run's budget_ms unenforceable: it is only
    * consulted AFTER probe_once returns, so a single unbounded attempt made the
    * whole budget moot. The per-attempt bound is what gives the budget meaning. */
   char *out = NULL;
   size_t out_len = 0;
   int rc = platform_exec_pipe_bounded(cmd, NULL, 0, &out, &out_len, timeout_ms, 64 * 1024);
   if (rc != 0)
   {
      free(out);
      return -1; /* not ready, failed, timed out, or over-talkative */
   }
   char buf[64] = "";
   if (out)
   {
      size_t n = out_len < sizeof(buf) - 1 ? out_len : sizeof(buf) - 1;
      memcpy(buf, out, n);
      buf[n] = '\0';
   }
   free(out);
   char *endp = NULL;
   long v = strtol(buf, &endp, 10);
   /* accept only a clean positive integer (trailing whitespace/newline ok) */
   while (endp && (*endp == '\n' || *endp == '\r' || *endp == ' ' || *endp == '\t'))
      endp++;
   if (!endp || *endp != '\0' || v <= 0)
      return -1;
   return (int)v;
}

/* db2_embedder_probe_fn: poll the embedder until two CONSECUTIVE reads agree on a
 * positive dim (guards a flapping load), or budget_ms elapses. ~2s between reads;
 * a mismatch or not-ready resets the streak and keeps polling within budget. */
static int embedder_probe_run(int *out_dim, int budget_ms, char *err, size_t errlen)
{
   long start = probe_mono_ms();
   int last = 0; /* last positive read; 0 = none yet */
   for (;;)
   {
      /* Give each attempt what is left of the budget, floored so a nearly-spent
       * budget still makes a real attempt rather than a zero-length one. */
      long spent = probe_mono_ms() - start;
      int remaining = (int)(budget_ms - spent);
      if (remaining < PROBE_ATTEMPT_MIN_MS)
         remaining = PROBE_ATTEMPT_MIN_MS;
      if (remaining > PROBE_ATTEMPT_MAX_MS)
         remaining = PROBE_ATTEMPT_MAX_MS;
      int d = probe_once(remaining);
      if (d > 0)
      {
         if (d == last)
         {
            if (out_dim)
               *out_dim = d;
            return 0; /* two consecutive equal positive reads -> stable */
         }
         last = d; /* first sighting (or changed) -> require one more match */
      }
      else
      {
         last = 0; /* not ready / error -> reset the streak */
      }
      if ((int)(probe_mono_ms() - start) >= budget_ms)
      {
         if (err && errlen)
            snprintf(err, errlen, "embedder not ready within %dms (cmd=%s)", budget_ms,
                     g_embed_cmd[0] ? g_embed_cmd : "(unset)");
         return -1;
      }
      struct timespec ts = {2, 0}; /* 2s between reads */
      nanosleep(&ts, NULL);
   }
}

void embedder_probe_register(const char *embed_command)
{
   if (!embed_command || !embed_command[0])
   {
      LOG_WARN("db2", "embedder dim probe: no embed command configured; §2b probe disabled");
      return;
   }
   snprintf(g_embed_cmd, sizeof(g_embed_cmd), "%s", embed_command);
   const char *env = getenv("AIMEE_DIM_PROBE_BUDGET_MS");
   if (env && env[0])
   {
      int ms = (int)strtol(env, NULL, 10);
      if (ms > 0)
         db2_set_dim_probe_budget_ms(ms);
   }
   db2_set_embedder_probe(embedder_probe_run);
}

void embedder_probe_unregister(void)
{
   db2_set_embedder_probe(NULL);
   g_embed_cmd[0] = '\0';
}
