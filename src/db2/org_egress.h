/* db2/org_egress.h: P2b-a durable egress authority state machine. */
#ifndef DEC_DB2_ORG_EGRESS_H
#define DEC_DB2_ORG_EGRESS_H 1

#include <stdint.h>

#define DB2_EGRESS_ERR_DENIED     (-2)
#define DB2_EGRESS_ERR_CONFLICT   (-3)
#define DB2_EGRESS_ADMITTED       0
#define DB2_EGRESS_REPLAY         1
#define DB2_EGRESS_RATE_REFUSED   2
#define DB2_EGRESS_BUDGET_REFUSED 3

typedef struct
{
   int outcome;
   int64_t dispatch_id;
   char state[16];
   char reserved_max_usd[64];
   char billable_model[201];
   int64_t pricing_version;
   char key_id[601];
   char vault_principal[601];
   char vault_agent[256];
   char vault_cred[256];
   int64_t max_input_tokens;
   int64_t max_output_tokens;
} db2_org_egress_admission_t;

int db2_org_egress_admit(const char *authority_id, const char *fingerprint, const char *issuer,
                         const char *serial, const char *origin, const char *request_id,
                         int64_t team, int has_project, int64_t project, const char *model_id,
                         const char *digest, int64_t lease_secs, db2_org_egress_admission_t *out);
int db2_org_egress_begin(const char *authority_id, const char *request_id, const char *owner_token,
                         const char *instance_id, int64_t ttl_secs, int64_t *out_id,
                         int64_t *out_generation);
int db2_org_egress_heartbeat(int64_t id, const char *owner_token, int64_t generation,
                             int64_t ttl_secs, int *out_ok);
/* Acquires and retains the dispatch-row lock in the caller's open tenant
 * transaction.  Commit/rollback releases the vendor-write fence. */
int db2_org_egress_owner_guard(int64_t id, const char *owner_token, int64_t generation,
                               int *out_ok);
int db2_org_egress_settle(int64_t id, const char *owner_token, int64_t generation,
                          const char *state, int http_status, int64_t prompt_tokens,
                          int64_t completion_tokens, int64_t cache_read_tokens,
                          int64_t cache_write_tokens, const char *outcome_class,
                          const char *settlement_basis, int *out_ok);
int db2_org_egress_recover(int limit, int64_t *out_count);

#endif
