/* P5-C3: ordinary-KB management action composition. */
#ifndef AIMEE_KB_MANAGEMENT_ACTION_H
#define AIMEE_KB_MANAGEMENT_ACTION_H

#include "modules/db2/c/management_action_journal.h"
#include "kb_management_health_exchange.h"
#include "kb_mgmt_token_authority_ipc.h"

#include <stddef.h>
#include <stdint.h>

#define KB_MANAGEMENT_ACTION_BODY_MAX     112U
#define KB_MANAGEMENT_ACTION_RESPONSE_MAX 128U

typedef enum
{
   KB_MANAGEMENT_ACTION_OK = 0,
   KB_MANAGEMENT_ACTION_NOT_FOUND,
   KB_MANAGEMENT_ACTION_DENIED,
   KB_MANAGEMENT_ACTION_CONFLICT,
   KB_MANAGEMENT_ACTION_UNAVAILABLE,
   KB_MANAGEMENT_ACTION_INTEGRITY,
   KB_MANAGEMENT_ACTION_INVALID,
   KB_MANAGEMENT_ACTION_INDETERMINATE
} kb_management_action_result_t;

typedef enum
{
   KB_MANAGEMENT_ACTION_NOT_SENT = 0,
   KB_MANAGEMENT_ACTION_SENT_RESPONSE,
   KB_MANAGEMENT_ACTION_SENT_AMBIGUOUS
} kb_management_action_transport_t;

typedef struct
{
   char action[14];
   char agent[64];
   char canonical[KB_MANAGEMENT_ACTION_BODY_MAX + 1];
   size_t canonical_len;
   uint8_t digest[32];
   char digest_hex[65];
} kb_management_action_body_t;

typedef db2_management_action_result_t (*kb_management_action_init_fn)(
    int64_t, const char *, db2_management_action_capability_t, const uint8_t[32], const char *,
    const char *, int, const char *, db2_management_action_operation_t *);
typedef db2_management_action_result_t (*kb_management_action_intent_fn)(
    const kb_principal_t *, const db2_management_action_operation_t *,
    db2_management_action_intent_t *);
typedef db2_management_action_result_t (*kb_management_action_outcome_fn)(
    const kb_principal_t *, const db2_management_action_outcome_operation_t *,
    db2_management_action_outcome_t *);
typedef kb_mgmt_token_authority_ipc_result_t (*kb_management_action_token_fn)(
    void *, const char *, const char *, kb_mgmt_token_authority_output_t *);
typedef kb_management_action_transport_t (*kb_management_action_request_fn)(
    void *, void *, const char *, const char *, const char *, const char *, uint64_t, char *,
    size_t, int *);

typedef struct
{
   kb_management_action_init_fn operation_init;
   kb_management_action_intent_fn intent_start;
   kb_management_action_outcome_fn outcome_append;
   void *snapshot_ctx;
   kb_management_health_snapshot_fn snapshot;
   void *bundle_ctx;
   kb_management_health_bundle_fn bundle_load;
   kb_management_health_bundle_clear_fn bundle_clear;
   void *server_ctx;
   kb_management_health_server_open_fn server_open;
   kb_management_action_request_fn server_request;
   kb_management_health_server_close_fn server_close;
   void *authority_ctx;
   kb_management_health_authority_fn authority_issue;
   void *token_ctx;
   kb_management_action_token_fn token_issue;
   void *clock_ctx;
   kb_management_health_clock_fn wall_seconds;
   kb_management_health_clock_fn monotonic_millis;
   const char *status_key_id;
   const unsigned char *status_public_key;
   const char *token_issuer;
   const char *kid;
   const char *installation_id;
   int ttl_seconds;
} kb_management_action_dependencies_t;

typedef struct
{
   const kb_principal_t *actor;
   int64_t team_id;
   const char *server_id;
   const char *body;
   size_t body_len;
   uint64_t deadline_millis;
} kb_management_action_request_t;

int kb_management_action_body_parse(const char *, size_t, kb_management_action_body_t *);
int kb_management_action_response_parse(const char *, size_t, int,
                                        db2_management_action_outcome_operation_t *);
kb_management_action_result_t
kb_management_action_execute(const kb_management_action_request_t *,
                             const kb_management_action_dependencies_t *);
kb_management_action_transport_t
kb_management_action_server_request_production(void *, void *, const char *, const char *,
                                               const char *, const char *, uint64_t, char *, size_t,
                                               int *);
kb_mgmt_token_authority_ipc_result_t
kb_management_action_token_issue_production(void *, const char *, const char *,
                                            kb_mgmt_token_authority_output_t *);

#endif
