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

#endif
