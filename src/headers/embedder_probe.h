/* embedder_probe.h -- embedder-runtime-fetch-autodim §2b: register the kb-side
 * embedder /health dim probe with the db2 layer (which stays config-free). The
 * probe shells `<embed_command> --dim` and polls until the embedder reports a
 * loaded model with a stable output dim, or the budget expires. */
#ifndef DEC_EMBEDDER_PROBE_H
#define DEC_EMBEDDER_PROBE_H 1

/* Capture the configured embed command + read the probe budget from
 * AIMEE_DIM_PROBE_BUDGET_MS (default 120000) and register the probe seam via
 * db2_set_embedder_probe + db2_set_dim_probe_budget_ms. Call once at kb startup,
 * BEFORE db2_init. A NULL/empty embed_command leaves the probe unregistered (the
 * §2b fresh-DB path then falls back to the default dim, as before). */
void embedder_probe_register(const char *embed_command);

/* Deregister the probe (call before db2_shutdown). */
void embedder_probe_unregister(void);

#endif /* DEC_EMBEDDER_PROBE_H */
