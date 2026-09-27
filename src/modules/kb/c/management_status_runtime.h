#ifndef AIMEE_KB_STORE_MANAGEMENT_STATUS_RUNTIME_H
#define AIMEE_KB_STORE_MANAGEMENT_STATUS_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#define KB_STORE_MANAGEMENT_STATUS_LOGIN_ROLE  "aimee_kb_status_login"
#define KB_STORE_MANAGEMENT_STATUS_ACTIVE_ROLE "aimee_kb_status"

enum
{
   KB_STORE_MANAGEMENT_STATUS_RUNTIME_OK = 0,
   KB_STORE_MANAGEMENT_STATUS_RUNTIME_DENIED = 1,
   KB_STORE_MANAGEMENT_STATUS_RUNTIME_CONFLICT = 2,
   KB_STORE_MANAGEMENT_STATUS_RUNTIME_ERROR = -1,
   KB_STORE_MANAGEMENT_STATUS_RUNTIME_INTEGRITY = -2,
};

typedef struct
{
   void *connection;
   int transaction_active;
} kb_store_management_status_runtime_t;

typedef struct
{
   int64_t seal_epoch;
   int sealed;
   char custody_key_id[601];
   char wire_key_id[65];
   unsigned char public_key[32];
   int enabled;
   int64_t version;
   unsigned char hwm_attestation[512];
   size_t hwm_attestation_len;
} kb_store_management_status_runtime_startup_t;

/* Open one authority-owned libpq connection, pin search_path to
 * pg_catalog,pg_temp and row_security to on, prove the fixed login role is
 * least-privileged, SET ROLE to the fixed status capability, and prove the
 * GUC/effective-role boundary again. No ambient KB_STORE connection is consulted. */
int kb_store_management_status_runtime_open(kb_store_management_status_runtime_t *,
                                            const char *conninfo, char *errbuf, size_t errlen);
void kb_store_management_status_runtime_close(kb_store_management_status_runtime_t *);

/* Explicit-connection alternative to the ordinary KB's ambient
 * kb_store_management_status_lookup(). */
int kb_store_management_status_runtime_lookup(kb_store_management_status_runtime_t *,
                                              const char *issuer, const char *serial_norm,
                                              const char *fingerprint, const char *target,
                                              const char *purpose, int64_t *generation,
                                              char *target_fingerprint,
                                              size_t target_fingerprint_len);
int kb_store_management_status_runtime_action_checkpoint(
    kb_store_management_status_runtime_t *, const char *peer_issuer, const char *peer_serial,
    const char *peer_fingerprint, const char *target, const char *caller_issuer,
    const char *caller_serial, const char *caller_fingerprint, int64_t staple_generation,
    int *revoked, int64_t *generation);

/* Hold the primary startup/seal snapshot transaction until startup_end. The
 * SQL facade returns the fixed registry binding and current attested version in
 * addition to the durable seal state. */
int kb_store_management_status_runtime_startup_begin(
    kb_store_management_status_runtime_t *, kb_store_management_status_runtime_startup_t *);
int kb_store_management_status_runtime_startup_end(kb_store_management_status_runtime_t *,
                                                   int commit);

#endif
