/* P5-B2c KB-only management-certificate lifecycle. */
#ifndef AIMEE_KB_MANAGEMENT_CERT_LIFECYCLE_H
#define AIMEE_KB_MANAGEMENT_CERT_LIFECYCLE_H

#include "modules/db2/c/management_client_instance.h"
#include "kb_pki.h"
#include "kb_workload_provider.h"

#include <stddef.h>
#include <stdint.h>

#define KB_MANAGEMENT_CERT_INSTALLATION_ID_LEN 32U
#define KB_MANAGEMENT_CERT_OPERATION_ID_LEN    64U
#define KB_MANAGEMENT_CERT_STORAGE_ID_LEN      32U
#define KB_MANAGEMENT_CERT_NONCE_LEN           32U
#define KB_MANAGEMENT_CERT_DIGEST_LEN          32U
#define KB_MANAGEMENT_CERT_PLAINTEXT_MAX       KB_WORKLOAD_UNWRAP_CAP

typedef enum
{
   KB_MANAGEMENT_CERT_OK = 0,
   KB_MANAGEMENT_CERT_DISABLED,
   KB_MANAGEMENT_CERT_UNAVAILABLE,
   KB_MANAGEMENT_CERT_DENIED,
   KB_MANAGEMENT_CERT_CONFLICT,
   KB_MANAGEMENT_CERT_INTEGRITY,
   KB_MANAGEMENT_CERT_INVALID
} kb_management_cert_result_t;

typedef struct
{
   char key_pem[KB_PKI_KEY_PEM_MAX];
   size_t key_pem_len;
   char leaf_pem[KB_PKI_CERT_PEM_MAX];
   size_t leaf_pem_len;
   char ca_pem[KB_PKI_CERT_PEM_MAX];
   size_t ca_pem_len;
} kb_management_cert_bundle_t;

typedef struct
{
   char installation_id[KB_MANAGEMENT_CERT_INSTALLATION_ID_LEN + 1];
   char lineage_id[KB_MANAGEMENT_CERT_INSTALLATION_ID_LEN + 1];
   int64_t generation;
   int64_t enrollment_id;
   int64_t not_before_epoch;
   int64_t not_after_epoch;
   int64_t revocation_generation;
   char issuer[DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   char serial_norm[DB2_MANAGEMENT_CLIENT_INSTANCE_SERIAL_MAX + 1];
   uint8_t fingerprint[KB_MANAGEMENT_CERT_DIGEST_LEN];
   uint8_t spki_digest[KB_MANAGEMENT_CERT_DIGEST_LEN];
   uint8_t public_bundle_digest[KB_MANAGEMENT_CERT_DIGEST_LEN];
} kb_management_cert_active_t;

typedef struct
{
   kb_workload_provider_t *provider;
   char installation_id[KB_MANAGEMENT_CERT_INSTALLATION_ID_LEN + 1];
   const char *custodied_ca_dir;
   const char *bundle_dir;
} kb_management_cert_config_t;

typedef struct kb_management_cert_lifecycle kb_management_cert_lifecycle_t;

#ifdef __cplusplus
extern "C"
{
#endif

   kb_management_cert_result_t
   kb_management_cert_lifecycle_open(const kb_management_cert_config_t *,
                                     kb_management_cert_lifecycle_t **);
   kb_management_cert_result_t kb_management_cert_reconcile(kb_management_cert_lifecycle_t *,
                                                            int64_t deadline_epoch,
                                                            kb_management_cert_active_t *);
   kb_management_cert_result_t kb_management_cert_load_active(kb_management_cert_lifecycle_t *,
                                                              kb_management_cert_bundle_t *,
                                                              kb_management_cert_active_t *);
   void kb_management_cert_bundle_clear(kb_management_cert_bundle_t *);
   void kb_management_cert_lifecycle_close(kb_management_cert_lifecycle_t *);

#ifdef __cplusplus
}
#endif

#endif
