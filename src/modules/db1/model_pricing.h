/* db1/model_pricing.h: server-owned per-model price table (DB1).
 *
 * The authoritative store for per-model / per-delegate token prices, kept on
 * aimee-server (DB1). Delegates default to 0 (subscription-priced) — an absent
 * row means "no DB1-stored price"; a present row (even 0) is an explicit operator
 * decision. token_estimate_cost consults this (via the server's db1-pricing bridge)
 * and lets a stored price override the static table / model_registry, so a metered
 * delegate's rate is configured here without touching code.
 *
 * Pure domain API. No backend types in any signature. */
#ifndef DEC_DB1_MODEL_PRICING_H
#define DEC_DB1_MODEL_PRICING_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Look up a model's stored price ($/MTok). Writes the rates into the out params
    * (each cleared to 0 first; either may be NULL). Returns 1 if a row exists (rates
    * set, possibly 0 = explicitly free), 0 if there is no stored price, -1 on bad
    * arg / DB error. */
   int db1_model_price_get(const char *model, double *in_per_mtok, double *out_per_mtok);

   /* Upsert a model's stored price (operator config). Returns 0 / -1. */
   int db1_model_price_set(const char *model, double in_per_mtok, double out_per_mtok);

   /* Remove a model's stored price, reverting it to static-table / model_registry
    * resolution. Returns 0 (deleted or already absent) / -1 on bad arg / DB error. */
   int db1_model_price_delete(const char *model);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_MODEL_PRICING_H */
