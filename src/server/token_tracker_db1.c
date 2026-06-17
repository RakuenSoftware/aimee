/* token_tracker_db1.c: installs token_tracker's DB1 server-owned price source.
 *
 * Linked into the server (and the pricing unit tests that exercise the bridge) so
 * token_estimate_cost can honour the authoritative per-model prices stored in DB1's
 * model_pricing table — making aimee-server the cost-of-record — without forcing a
 * hard token_tracker -> db1 link on the many unrelated binaries that link
 * token_tracker.o. A constructor wires it up automatically in any binary that links
 * this TU. */
#include "token_tracker.h"
#include "db1/model_pricing.h"

static int db1_price(const char *model, double *in_per_mtok, double *out_per_mtok)
{
   /* token_tracker treats a return of 1 as "stored price present" → authoritative
    * override (even 0). db1_model_price_get returns 1 only when a row exists. */
   return db1_model_price_get(model, in_per_mtok, out_per_mtok) == 1 ? 1 : 0;
}

__attribute__((constructor)) static void token_tracker_db1_install(void)
{
   token_tracker_set_db1_price_fn(db1_price);
}
