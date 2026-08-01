#ifndef DEC_DASHBOARD_H
#define DEC_DASHBOARD_H 1

#include "aimee_features.h"
#include <stddef.h>

#define CORS_ORIGIN_LEN  256
#define CORS_MAX_ORIGINS 32

/* Start the local dashboard HTTP server. Blocks indefinitely.
 * Port defaults to 9200 if <= 0. */
void dashboard_serve(int port);

/* CORS origin management */
int dashboard_cors_add(const char *origin);
int dashboard_cors_remove(const char *origin);
int dashboard_cors_list(char origins[][CORS_ORIGIN_LEN], int max);

/* Dashboard JSON API handlers (return malloc'd JSON strings, caller frees) */
char *api_delegations(void);
char *api_metrics(void);
char *api_vector_status(void);
char *api_traces(void);
char *api_memory_stats(void);
char *api_plans(void);
char *api_logs(void);
char *api_dashboard_reminders(void);
char *api_dashboard_recall(void);
char *api_dashboard_directives(void);
char *api_dashboard_maintenance(void);
/* Identity panel: operator charter + learned working-profile state. */
char *api_dashboard_identity(void);
/* Onboard / readiness panel: same JSON shape as `aimee onboard --json`
 * with skip_setup=1 so the dashboard surface is read-only. */
char *api_dashboard_onboard(void);
char *api_token_audit(void);
char *api_bench_results(void);
char *api_doctor(void);
char *api_payload_rewrite_status(void);

/* pam_check_credentials moved to headers/pam_auth.h (posix/pam_auth.c), which
 * dashboard.c includes: aimee-kb needs the same check and does not link the
 * dashboard. */

/* Base64 decode (minimal, for Authorization header). Returns decoded length. */
int base64_decode(const char *in, char *out, size_t out_len);

#endif /* DEC_DASHBOARD_H */
