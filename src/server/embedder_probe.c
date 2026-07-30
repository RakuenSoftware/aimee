/* embedder_probe.c -- §2b kb-side embedder /health dim probe. Registered with the
 * db2 layer so db2_init can derive a fresh DB's embedding dim from the running
 * embedder without db2 learning the embed transport (db2 stays config-free). */
#include "embedder_probe.h"

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

static long probe_mono_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* One probe attempt: run `<embed_cmd> --dim`, return the parsed positive dim, or
 * <=0 if the embedder is not ready / the command failed / output was not a single
 * positive integer. */
static int probe_once(void)
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
      int d = memory_embed_text("dim probe", g_embed_cmd, EMBED_INPUT_DOCUMENT, vec, EMBED_MAX_DIM);
      return d > 0 ? d : -1;
   }
   char cmd[1100];
   snprintf(cmd, sizeof(cmd), "%s --dim", g_embed_cmd);
   FILE *p = popen(cmd, "r");
   if (!p)
      return -1;
   char buf[64] = "";
   size_t n = fread(buf, 1, sizeof(buf) - 1, p);
   buf[n] = '\0';
   int status = pclose(p);
   if (status != 0)
      return -1; /* command signalled "not ready" / error */
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
      int d = probe_once();
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

/* db2_embedder_serving_probe_fn: ask the endpoint which VECTOR SPACE it serves.
 *
 * POLLS, because the in-container embedder is a sibling process the entrypoint starts
 * beside the kb: it is reliably not serving yet when the kb boots, and a one-shot probe
 * therefore failed on every cold start and left the guard inactive — the hole the guard
 * exists to close. Retries only a TRANSPORT failure; a reachable endpoint that reports no
 * identity returns empty immediately (a legacy embedder, guard stays inactive by design,
 * and retrying that would just delay every such boot).
 *
 * The budget is modest on purpose. db2_init calls this after the dim probe has already
 * waited for readiness, so in practice the first read succeeds; the window only covers a
 * restart where no dim probe runs because the dim is already recorded. Exhausting it
 * leaves the guard inactive for this start rather than holding the kb down. */
#define SERVING_PROBE_BUDGET_MS 60000
#define SERVING_PROBE_INTERVAL_MS 2000

static int embedder_probe_serving_id(char *out, size_t out_len, char *err, size_t errlen)
{
   if (!out || out_len == 0)
      return -1;
   out[0] = '\0';
   long start = probe_mono_ms();
   for (;;)
   {
      if (memory_embed_serving_id(g_embed_cmd, out, out_len) == 0)
      {
         if (out[0])
            LOG_INFO("db2", "embedder serving identity: %s", out);
         else
            LOG_INFO("db2", "embedder reports no serving identity; vector-space guard inactive");
         return 0;
      }
      if ((int)(probe_mono_ms() - start) >= SERVING_PROBE_BUDGET_MS)
      {
         if (err && errlen)
            snprintf(err, errlen, "embedder unreachable within %dms (cmd=%s)",
                     SERVING_PROBE_BUDGET_MS, g_embed_cmd[0] ? g_embed_cmd : "(unset)");
         return -1;
      }
      struct timespec ts = {SERVING_PROBE_INTERVAL_MS / 1000, 0};
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
   /* Registered, not called: the identity is fetched inside db2_init, once the embedder
    * has had the dim probe's patience applied to it. */
   db2_set_embedder_serving_probe(embedder_probe_serving_id);
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
   /* Both seams point at this translation unit's statics, so both have to go before
    * db2_shutdown — leaving one registered would hand db2 a callback over a cleared
    * g_embed_cmd. */
   db2_set_embedder_serving_probe(NULL);
   g_embed_cmd[0] = '\0';
}
