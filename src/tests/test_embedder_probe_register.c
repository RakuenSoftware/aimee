/* test_embedder_probe_register.c: which probes get registered for which embed command.
 *
 * This tests a registration DECISION, not a computation, because that decision is where
 * the vector-space guard was lost.
 *
 * The chain is four links: kb_main registers the probes -> db2_init calls the serving
 * probe -> the probe asks memory_embed_serving_id what space is being served -> the guard
 * records or refuses. Only the last link was tested. It was also the only link that was
 * never broken: test_embedding_dim.c asserts by name that the guard refuses a
 * builtin -> bundled-model switch.
 *
 * Link one was broken. kb_main skipped embedder_probe_register ENTIRELY when the
 * configured embedder was "builtin", on the correct grounds that the DIM probe cannot
 * work against a fixed-width embedder with no /health -- it would spin for its whole
 * budget on every boot. One call registered two probes, so the serving-identity probe,
 * which answers for the builtin from a constant with no network at all, went down with
 * it. A corpus embedded by the builtin therefore recorded no identity, and selecting the
 * bundled model found kb_meta empty and adopted the model's identity over lexical
 * vectors. Both are 384-dim, so the dim guard is silent by construction, and the guard
 * written for exactly this case was never asked.
 *
 * WHAT EACH ASSERTION BELOW IS WORTH, honestly:
 *
 *   - "no dim probe for the builtin" FAILS against the unmodified module. It is the
 *     enabling change: it is what makes an unconditional call site safe, and without it
 *     removing kb_main's gate would stall every builtin boot on the dim probe.
 *   - "the serving probe IS registered for the builtin" PASSES against the unmodified
 *     module, because embedder_probe_register was always right about it. It is a
 *     characterization test: it pins down behavior that was correct but unreachable, so
 *     that reintroducing a caller-side gate has something to break.
 *
 * The defect itself lived at the call site, where kb_main now makes no decision at all,
 * so there is nothing left there for a unit test to hold. What these tests can and do
 * establish is that the module is safe to call unconditionally for every embed command,
 * which is the property the fix depends on.
 */
#include "../modules/db2/c/lifecycle.h"
#include "../modules/memory/memory_core_internal.h"
#include "embedder_probe.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(int ok, const char *what)
{
   printf(ok ? "  ok    %s\n" : "  FAIL  %s\n", what);
   if (!ok)
      failures++;
}

/* Registration is global state on the db2 side, so each case starts from nothing.
 * Without this a later case could pass on an earlier one's registration. */
static void reset(void)
{
   embedder_probe_unregister();
   assert(!db2_embedder_probe_registered());
   assert(!db2_embedder_serving_probe_registered());
}

int main(void)
{
   printf("no embed command configured\n");
   reset();
   embedder_probe_register(NULL);
   check(!db2_embedder_probe_registered(), "NULL command registers no dim probe");
   check(!db2_embedder_serving_probe_registered(), "NULL command registers no serving probe");

   reset();
   embedder_probe_register("");
   check(!db2_embedder_probe_registered(), "empty command registers no dim probe");
   check(!db2_embedder_serving_probe_registered(), "empty command registers no serving probe");

   /* "builtin" was a real embedder here once — a lexical feature hash that served when
    * nothing was configured. It is gone, and with it the idea that an unconfigured kb
    * has a vector space at all: no command means no probes, asserted above. */

   printf("a sidecar embed command\n");
   reset();
   embedder_probe_register("python3 /opt/aimee/scripts/embed-remote.py");
   check(db2_embedder_probe_registered(), "dim probe registered");
   check(db2_embedder_serving_probe_registered(), "serving probe registered");

   printf("an http endpoint\n");
   reset();
   embedder_probe_register("http://127.0.0.1:8760");
   check(db2_embedder_probe_registered(), "dim probe registered");
   check(db2_embedder_serving_probe_registered(), "serving probe registered");

   printf("unregister clears both seams\n");
   /* Both point at embedder_probe.c's statics, so leaving either registered would hand
    * db2 a callback over a cleared command. */
   embedder_probe_unregister();
   check(!db2_embedder_probe_registered(), "dim probe cleared");
   check(!db2_embedder_serving_probe_registered(), "serving probe cleared");

   if (failures)
   {
      printf("\nembedder_probe_register: %d check(s) failed\n", failures);
      return 1;
   }
   printf("\nembedder_probe_register: ok\n");
   return 0;
}
