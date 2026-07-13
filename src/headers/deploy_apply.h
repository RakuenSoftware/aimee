#ifndef DEPLOY_APPLY_H
#define DEPLOY_APPLY_H 1

#include <stddef.h>

/* deploy_apply — server-orchestrated container deploy.
 *
 * When aimee-server runs with the host Docker socket mounted, the setup wizard can
 * bring up the managed sibling services (postgres + aimee-kb + aimee-llm) itself:
 * the operator deploys ONE container (aimee-server) and the finished wizard spins
 * up the rest. The wizard's page-2 config is translated by config_emit_deploy_env
 * into the compose env (COMPOSE_PROFILES + AIMEE_LLM_* + AIMEE_KB_API_* …), and
 * this module runs `docker compose -f <managed-file> up -d` with that env against
 * the mounted socket.
 *
 * `up -d` may pull multi-GB images, so the apply runs on a BACKGROUND THREAD (the
 * HTTP listener is single-threaded and must not block for minutes). The wizard
 * kicks it off, then polls status. */

/* 1 iff server-orchestrated deploy is enabled: AIMEE_DEPLOY_ENABLED=1 (the deploy
 * compose mounts the Docker socket + managed compose file and sets this). */
int deploy_apply_enabled(void);

/* The managed compose file path: AIMEE_DEPLOY_COMPOSE_FILE, else the built-in
 * default (/opt/aimee/deploy/aimee-managed.compose.yaml). Writes into out. */
void deploy_apply_compose_file(char *out, size_t cap);

/* Start a background `docker compose up -d` for the services the current config
 * selects. Returns 0 when a new deploy was started, 1 when one is already running
 * (no-op), -1 on failure to start the worker. Non-blocking. */
int deploy_apply_start(void);

/* Snapshot the background deploy's state. `running` is 1 while the worker runs.
 * `last_exit` is the compose exit code of the most recent completed run (INT_MIN
 * if none yet). `out` receives the tail of the most recent run's combined
 * stdout+stderr. Any out param may be NULL. */
void deploy_apply_state(int *running, int *last_exit, char *out, size_t out_cap);

/* Run `docker compose -f <managed-file> ps --format json` synchronously (fast —
 * no pulls) and copy its stdout into out. *exit_code gets the compose exit status.
 * Returns 0 on success, -1 on spawn/read failure. */
int deploy_apply_status(char *out, size_t out_cap, int *exit_code);

#endif /* DEPLOY_APPLY_H */
