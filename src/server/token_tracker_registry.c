/* token_tracker_registry.c: installs token_tracker's registry-price fallback.
 *
 * Linked into the server (and the pricing unit tests) so token_estimate_cost can
 * price providers absent from the static token_tracker table (gemini, groq,
 * mistral) and honour models.dev / operator pricing overrides — making
 * token_estimate_cost the single cost authority — without forcing a hard
 * token_tracker -> model_registry link dependency on the many unrelated binaries
 * that link token_tracker.o. The hook is installed by a constructor so any binary
 * that links this TU wires it up automatically. */
#include "token_tracker.h"
#include "model_registry.h"

static int registry_price(const char *model, double *in_per_mtok, double *out_per_mtok)
{
   if (!model || !model[0])
      return 0;
   model_capability_t cap;
   if (!model_capability_get(NULL, model, &cap))
      return 0;
   if (in_per_mtok)
      *in_per_mtok = cap.cost_in_per_mtok;
   if (out_per_mtok)
      *out_per_mtok = cap.cost_out_per_mtok;
   return 1;
}

__attribute__((constructor)) static void token_tracker_registry_install(void)
{
   token_tracker_set_registry_price_fn(registry_price);
}
