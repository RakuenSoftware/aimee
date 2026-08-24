/* kb_service_kb.h: prototypes for the kb.* dispatch handlers
 * defined in kb_service_kb.c.  Included from kb_service.c so the
 * dispatch table can call them. */
#ifndef DEC_KB_SERVICE_KB_H
#define DEC_KB_SERVICE_KB_H 1

#include "cJSON.h"

char *kb_service_status_json(const char *project);
char *kb_service_health_json(void);
char *kb_service_ingest_status_json(void);
int kb_handle_ingest(int fd, cJSON *req);
int kb_handle_maintenance_run(int fd, cJSON *req);
int kb_handle_kb_export(int fd, cJSON *req);
int kb_handle_kb_import(int fd, cJSON *req);

int kb_handle_file_get(int fd, cJSON *req);

/* S6: register the plan-advisory arms and install the bandit-backed sampler
 * and reward sink. Called once at service start. */
void kb_policy_arms_init(void);

#endif /* DEC_KB_SERVICE_KB_H */
