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

/* Effective embedding dim: AIMEE_EMBEDDING_DIM env override (1..EMBED_MAX_DIM)
 * when set, else cfg->embedding_dim. Pass the result to db2_set_embedding_dim()
 * so the schema columns match the running embedder. Non-mutating. */
int config_resolve_embedding_dim(const config_t *cfg);

/* embedder-runtime-fetch-autodim §2a: 1 iff the operator pinned a positive
 * embedding dim — defined as config_resolve_embedding_dim(cfg) > 0, so "pinned"
 * is exactly consistent with the value db2_set_embedding_dim receives. A pinned
 * dim is authoritative; an UNpinned deployment lets the recorded
 * kb_meta.schema_embedding_dim win over the default. AIMEE_EMBEDDING_DIM="0" /
 * non-numeric / empty is NOT a pin (config_resolve already maps it to 0), nor is
 * an unset cfg->embedding_dim. Pass to db2_set_embedding_dim_pinned(). */
int config_embedding_dim_is_pinned(const config_t *cfg);

/* Emit the deploy-time environment the compose stack consumes for the page-2
 * backend record (setup wizard). Writes shell-sourceable KEY=VALUE lines to buf:
 *   COMPOSE_PROFILES  — which optional services to bring up ("kb" for a local kb,
 *                       "llm" when any LLM role is local; empty for a remote kb).
 *   AIMEE_LLM_<ROLE>_MODE/TIER/URL — per Phase-0 plugin env (local => TIER,
 *                       external => URL, off => mode=off).
 *   AIMEE_LLM_URL     — the compose aimee-llm service, when any role is local.
 *   AIMEE_EMBEDDING_DIM — only when the operator pinned it (external embedder);
 *                       a local/unset dim is derived from the embedder /health.
 *   AIMEE_KB_API_URL/BEARER — when kb_mode=remote (connect out, deploy nothing).
 * Pure/non-mutating; the single translation both the entrypoint and an operator
 * `compose up` wrapper source. */
void config_emit_deploy_env(const config_t *cfg, char *buf, size_t n);

#endif
