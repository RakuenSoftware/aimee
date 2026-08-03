#ifndef DEC_CONFIG_DATABASE_H
#define DEC_CONFIG_DATABASE_H 1

/* Parses the DB2 config block (db2_url, db2_pool_size). DB1 path and
 * other tier-specific config live elsewhere; pgvector lives inside DB2
 * and needs no separate connection URL. */

#include "config.h"
#include "cJSON.h"

void config_parse_database(config_t *cfg, cJSON *root);

/* Overlays the AIMEE_DB2_URL environment variable onto cfg->db2_url when it is
 * set and non-empty; returns 1 if applied, 0 otherwise. AIMEE_DB2_URL is the
 * source of truth in container deploys (the runtime injects the current
 * Postgres address every start), so a db2_url persisted in aimee.yaml on an
 * earlier boot goes stale when Postgres is recreated on a new bridge IP.
 * Callers in the aimee-kb DB2 resolution paths apply this AFTER config_load so
 * the env wins over the cached file value. Deliberately NOT called from
 * config_load itself — only where the kb resolves its own DB2 connection — so
 * other config consumers are unaffected. */
int config_apply_db2_url_env_override(config_t *cfg);

/* The db2 URL this process should dial: AIMEE_DB2_URL when set, else the stored
 * db2_url. Written into caller storage (it carries a credential and outlives any
 * shared buffer). Returns 1 when non-empty. */
int config_db2_url_effective(char *out, size_t n);

/* THE embedding width. This is the only place in the tree the number is written:
 * selecting an embedder records its width in config (bekko-a25m -> 384), the env
 * and the wizard change it through the accessors above, and everything else —
 * schema sizing, the DB2 columns, the builtin lexical embedder, the doctor's
 * expected-dim report — reads it from here.
 *
 * It is a config concern because config is what a deployment can change. Any
 * caller keeping its own fallback recreates the bug this replaced: kb_main used
 * to default to 1024 while the bundled model returned 384, so an unpinned kb
 * sized its columns for one embedder and then inserted vectors from another. */
int config_embedder_dims_default(void);

/* Effective embedding dim: EMBEDDER_DIMS env override (1..EMBED_MAX_DIM)
 * when set, else cfg->embedder_dims. 0 means "nothing pinned" — that is load-
 * bearing for the §2a precedence below, so this deliberately does NOT apply the
 * default. Callers that need a usable width (not a pin signal) want
 * config_embedder_dims_effective(). Non-mutating. */
int config_resolve_embedder_dims(const config_t *cfg);

/* No-arg form of the above, against the loaded config. Still the PIN signal
 * (0 = nothing pinned), NOT the effective width -- see config_embedder_dims_current. */
int config_resolve_embedder_dims_current(void);

/* The width to actually embed and size columns with: the pin when there is one,
 * else config_embedder_dims_default(). Pass this to db2_set_embedding_dim() —
 * that layer holds no default of its own. Non-mutating. */
int config_embedder_dims_effective(const config_t *cfg);

/* No-arg form for callers holding no config_t (the CLI doctor). Same value as
 * config_embedder_dims_effective against the loaded config. */
int config_embedder_dims_current(void);

/* THE synthesis endpoint. One config field (llm_synth_endpoint), one resolver, and
 * the SYNTHESIS_ENDPOINT env override applied in one place — the same shape as the
 * embedder address (config_embedder_command) and the embedding width
 * (config_embedder_dims_*).
 *
 * Writes the OpenAI chat base into out ("{endpoint}/v1"), appending /v1 only when the
 * configured value did not already include it, so an operator may supply either form.
 * Returns 1 when an endpoint is configured, 0 when none is (the caller's stage then
 * stays idle). Never partially fills out.
 *
 * Synthesis is external-only: the aimee-llm container this used to name is retired, so
 * there is no local default to fall back to. Non-mutating. */
int config_synth_chat_endpoint(const config_t *cfg, char *out, size_t out_len);

/* Same resolver without a config_t: reads llm_synth_endpoint through its accessor.
 * SYNTHESIS_ENDPOINT still outranks the stored field, and normalization is shared. */
int config_synth_chat_endpoint_current(char *out, size_t out_len);

/* embedder-runtime-fetch-autodim §2a: 1 iff the operator pinned a positive
 * embedding dim — defined as config_resolve_embedder_dims(cfg) > 0, so "pinned"
 * is exactly consistent with the value db2_set_embedding_dim receives. A pinned
 * dim is authoritative; an UNpinned deployment lets the recorded
 * kb_meta.schema_embedding_dim win over the default. EMBEDDER_DIMS="0" /
 * non-numeric / empty is NOT a pin (config_resolve already maps it to 0), nor is
 * an unset cfg->embedder_dims. Pass to db2_set_embedding_dim_pinned(). */
int config_embedder_dims_is_pinned(const config_t *cfg);

/* No-arg form for callers holding no config_t; same answer as
 * config_embedder_dims_is_pinned against the live config. Prefer this. */
int config_embedder_dims_pinned_current(void);

/* Emit the deploy-time environment the compose stack consumes for the page-2
 * backend record (setup wizard). Writes shell-sourceable KEY=VALUE lines to buf:
 *   COMPOSE_PROFILES  — which optional services to bring up ("kb" for a local kb,
 *                       "llm" when any LLM role is local; empty for a remote kb).
 *   AIMEE_LLM_<ROLE>_MODE/TIER/URL — per Phase-0 plugin env (local => TIER,
 *                       external => URL, off => mode=off).
 *   SYNTHESIS_ENDPOINT     — the compose aimee-llm service, when any role is local.
 *   EMBEDDER_DIMS — only when the operator pinned it (external embedder);
 *                       a local/unset dim is derived from the embedder /health.
 *   AIMEE_KB_API_URL/BEARER — when kb_mode=remote (connect out, deploy nothing).
 * Pure/non-mutating; the single translation both the entrypoint and an operator
 * `compose up` wrapper source. */
void config_emit_deploy_env(const config_t *cfg, char *buf, size_t n);

/* Same emitter, reading the live config itself. */
void config_emit_deploy_env_current(char *buf, size_t n);

#endif
