#ifndef AIMEE_DB2_MANAGEMENT_STATUS_RUNTIME_H
#define AIMEE_DB2_MANAGEMENT_STATUS_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#define DB2_MANAGEMENT_STATUS_LOGIN_ROLE  "aimee_kb_status_login"
#define DB2_MANAGEMENT_STATUS_ACTIVE_ROLE "aimee_kb_status"

enum
{
   DB2_MANAGEMENT_STATUS_RUNTIME_OK = 0,
   DB2_MANAGEMENT_STATUS_RUNTIME_DENIED = 1,
   DB2_MANAGEMENT_STATUS_RUNTIME_CONFLICT = 2,
   DB2_MANAGEMENT_STATUS_RUNTIME_ERROR = -1,
   DB2_MANAGEMENT_STATUS_RUNTIME_INTEGRITY = -2,
};

typedef struct
{
   void *connection;
   int transaction_active;
} db2_management_status_runtime_t;

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
} db2_management_status_runtime_startup_t;

/* Open one authority-owned libpq connection, pin search_path to
 * pg_catalog,pg_temp and row_security to on, prove the fixed login role is
 * least-privileged, SET ROLE to the fixed status capability, and prove the
 * GUC/effective-role boundary again. No ambient DB2 connection is consulted. */
int db2_management_status_runtime_open(db2_management_status_runtime_t *, const char *conninfo,
                                       char *errbuf, size_t errlen);
void db2_management_status_runtime_close(db2_management_status_runtime_t *);

/* Explicit-connection alternative to the ordinary KB's ambient
 * db2_management_status_lookup(). */
int db2_management_status_runtime_lookup(db2_management_status_runtime_t *, const char *issuer,
                                         const char *serial_norm, const char *fingerprint,
                                         const char *target, const char *purpose,
                                         int64_t *generation, char *target_fingerprint,
                                         size_t target_fingerprint_len);
int db2_management_status_runtime_action_checkpoint(
    db2_management_status_runtime_t *, const char *peer_issuer, const char *peer_serial,
    const char *peer_fingerprint, const char *target, const char *caller_issuer,
    const char *caller_serial, const char *caller_fingerprint, int64_t staple_generation,
    int *revoked, int64_t *generation);

/* Hold the primary startup/seal snapshot transaction until startup_end. The
 * SQL facade returns the fixed registry binding and current attested version in
 * addition to the durable seal state. */
int db2_management_status_runtime_startup_begin(db2_management_status_runtime_t *,
                                                db2_management_status_runtime_startup_t *);
int db2_management_status_runtime_startup_end(db2_management_status_runtime_t *, int commit);

#endif
