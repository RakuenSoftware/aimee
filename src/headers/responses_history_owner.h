/* Generic process-local Responses history transport. No storage or authority. */
#ifndef AIMEE_RESPONSES_HISTORY_OWNER_H
#define AIMEE_RESPONSES_HISTORY_OWNER_H
struct cJSON;
typedef struct {
    unsigned contract_version;
    void *context; /* Installed owner keeps this immutable for serving lifetime. */
    /* 0 committed/handled; 1 unselected; -1 refused/unknown. Selectors and strings
     * are not identity proof; the owner must use its actual authenticated host. */
    int (*save)(void *, const char *response_id, const char *http_scope, const char *actor);
    int (*load)(void *, const char *response_id, const char *http_scope, const char *actor,
                struct cJSON **history);
} aimee_responses_history_owner_v1;
/* Startup only. One explicit owner, no replacement/dynamic alias selection. */
int aimee_responses_history_owner_install_v1(const aimee_responses_history_owner_v1 *);
int aimee_responses_history_owner_save_v1(const char *, const char *, const char *);
int aimee_responses_history_owner_load_v1(const char *, const char *, const char *,struct cJSON **);
#endif
