/* P5-B3a: bounded reverse-management health exchange (no route wiring). */
#ifndef AIMEE_KB_MANAGEMENT_HEALTH_EXCHANGE_H
#define AIMEE_KB_MANAGEMENT_HEALTH_EXCHANGE_H

#include "kb_management_cert_lifecycle.h"
#include "kb_identity.h"
#include "kb_mgmt_status.h"
#include "modules/db2/c/server_registry.h"
#include "kb_mgmt_client.h"

#include <stddef.h>
#include <stdint.h>

#define KB_MANAGEMENT_HEALTH_RESPONSE_MAX 4096U

typedef enum
{
   KB_MANAGEMENT_HEALTH_OK = 0,
   KB_MANAGEMENT_HEALTH_NOT_FOUND,
   KB_MANAGEMENT_HEALTH_DENIED,
   KB_MANAGEMENT_HEALTH_CONFLICT,
   KB_MANAGEMENT_HEALTH_UNAVAILABLE,
   KB_MANAGEMENT_HEALTH_INTEGRITY,
   KB_MANAGEMENT_HEALTH_INVALID
} kb_management_health_result_t;

typedef kb_management_health_result_t (*kb_management_health_snapshot_fn)(void *,
                                                                          const kb_principal_t *,
                                                                          int64_t, const char *,
                                                                          db2_server_snapshot_t *);
typedef kb_management_health_result_t (*kb_management_health_bundle_fn)(
    void *, kb_management_cert_bundle_t *, kb_management_cert_active_t *);
typedef void (*kb_management_health_bundle_clear_fn)(void *, kb_management_cert_bundle_t *);
typedef kb_management_health_result_t (*kb_management_health_server_open_fn)(
    void *, const db2_server_snapshot_t *, const kb_management_cert_bundle_t *, uint64_t, void **);
typedef kb_management_health_result_t (*kb_management_health_server_request_fn)(
    void *, void *, const char *, const char *, const char *, const char *, uint64_t, char *,
    size_t, int *);
typedef void (*kb_management_health_server_close_fn)(void *, void *);
typedef kb_management_health_result_t (*kb_management_health_authority_fn)(
    void *, const kb_management_cert_bundle_t *, const char *, size_t, uint64_t, char *, size_t,
    int *);
typedef uint64_t (*kb_management_health_clock_fn)(void *);

typedef struct
{
   void *snapshot_ctx;
   kb_management_health_snapshot_fn snapshot;
   void *bundle_ctx;
   kb_management_health_bundle_fn bundle_load;
   kb_management_health_bundle_clear_fn bundle_clear;
   void *server_ctx;
   kb_management_health_server_open_fn server_open;
   kb_management_health_server_request_fn server_request;
   kb_management_health_server_close_fn server_close;
   void *authority_ctx;
   kb_management_health_authority_fn authority_issue;
   void *clock_ctx;
   kb_management_health_clock_fn wall_seconds;
   kb_management_health_clock_fn monotonic_millis;
   const char *status_key_id;
   const unsigned char *status_public_key;
} kb_management_health_dependencies_t;

typedef struct
{
   const kb_principal_t *actor;
   int64_t team_id;
   const char *server_id;
   uint64_t deadline_millis;
} kb_management_health_request_t;

kb_management_health_result_t
kb_management_health_exchange(const kb_management_health_request_t *,
                              const kb_management_health_dependencies_t *);

/* Length-aware private wire codecs exposed for focused tests/fuzzing. */
int kb_management_health_challenge_decode(const char *, size_t,
                                          unsigned char[KB_MGMT_STATUS_NONCE_LEN], uint64_t *);
int kb_management_read_challenge_decode(const char *, size_t, const char *,
                                        unsigned char[KB_MGMT_STATUS_NONCE_LEN], uint64_t *);
int kb_management_health_response_decode(const char *, size_t, const char *);

/* Production primary-snapshot and lifecycle adapters. */
kb_management_health_result_t kb_management_health_snapshot_primary(void *, const kb_principal_t *,
                                                                    int64_t, const char *,
                                                                    db2_server_snapshot_t *);
kb_management_health_result_t kb_management_health_bundle_active(void *,
                                                                 kb_management_cert_bundle_t *,
                                                                 kb_management_cert_active_t *);
void kb_management_health_bundle_cleanse(void *, kb_management_cert_bundle_t *);

typedef struct
{
   const char *server_ca_pem;
} kb_management_health_server_config_t;
kb_management_health_result_t
kb_management_health_server_open_production(void *, const db2_server_snapshot_t *,
                                            const kb_management_cert_bundle_t *, uint64_t, void **);
kb_management_health_result_t
kb_management_health_server_request_production(void *, void *, const char *, const char *,
                                               const char *, const char *, uint64_t, char *, size_t,
                                               int *);
void kb_management_health_server_close_production(void *, void *);

#endif
