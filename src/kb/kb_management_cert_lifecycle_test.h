#ifndef AIMEE_KB_MANAGEMENT_CERT_LIFECYCLE_TEST_H
#define AIMEE_KB_MANAGEMENT_CERT_LIFECYCLE_TEST_H

#include "kb_management_cert_lifecycle.h"
#include "kb_management_cert_storage.h"

typedef enum
{
   KB_MANAGEMENT_CERT_CRASH_AFTER_PREPARE = 1,
   KB_MANAGEMENT_CERT_CRASH_AFTER_INTENT,
   KB_MANAGEMENT_CERT_CRASH_AFTER_PENDING,
   KB_MANAGEMENT_CERT_CRASH_AFTER_BEGIN,
   KB_MANAGEMENT_CERT_CRASH_AFTER_CANDIDATE,
   KB_MANAGEMENT_CERT_CRASH_AFTER_ACTIVATE,
   KB_MANAGEMENT_CERT_CRASH_AFTER_PROMOTE,
   KB_MANAGEMENT_CERT_CRASH_BEFORE_TERMINAL_CLEAR
} kb_management_cert_crash_point_t;

typedef struct
{
   int64_t (*now)(void *);
   int (*random)(void *, uint8_t *, size_t);
   kb_workload_result_t (*attest)(void *, const uint8_t[32], const uint8_t[32],
                                  kb_workload_identity_t *);
   kb_workload_result_t (*wrap)(void *, const uint8_t[32], const uint8_t[32], const void *, size_t,
                                kb_workload_identity_t *, uint8_t *, size_t, size_t *);
   kb_workload_result_t (*unwrap)(void *, const uint8_t[32], const uint8_t[32], const void *,
                                  size_t, kb_workload_identity_t *, uint8_t *, size_t, size_t *);
   db2_management_client_instance_result_t (*preflight)(
       void *, const db2_management_client_grant_preflight_request_t *,
       db2_management_client_grant_preflight_t *);
   db2_management_client_instance_result_t (*begin_initial)(
       void *, const db2_management_client_initial_request_t *, db2_management_client_pending_t *);
   db2_management_client_instance_result_t (*begin_renewal)(
       void *, const db2_management_client_renewal_request_t *, db2_management_client_pending_t *);
   db2_management_client_instance_result_t (*activate)(
       void *, const db2_management_client_activation_request_t *, db2_management_client_active_t *);
   db2_management_client_instance_result_t (*snapshot)(
       void *, const char[33], const db2_management_client_instance_binding_t *,
       db2_management_client_active_t *);
   int (*crash)(void *, kb_management_cert_crash_point_t);
   int (*arena_fail)(void *, int);
} kb_management_cert_test_ops_t;

#ifdef AIMEE_MANAGEMENT_CERT_TESTING
kb_management_cert_result_t kb_management_cert_lifecycle_open_for_test(
    const kb_management_cert_config_t *, kb_workload_provider_kind_t, int,
    const kb_management_cert_test_ops_t *, void *, kb_management_cert_lifecycle_t **);
#endif

#endif
