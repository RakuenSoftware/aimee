#ifndef AIMEE_DB2_MANAGEMENT_STATUS_KEY_H
#define AIMEE_DB2_MANAGEMENT_STATUS_KEY_H

#include "org_vault_key_use.h"
#include "management_status_runtime.h"

typedef struct
{
   void *connection;
   int owns_connection;
   int transaction_active;
} db2_management_status_key_ctx_t;

/* The authority owns a dedicated libpq connection; it never borrows DB2's
 * thread-local runtime connection. close rolls back an outstanding guard. */
int db2_management_status_key_ctx_open(db2_management_status_key_ctx_t *, const char *conninfo,
                                       char *errbuf, size_t errlen);
/* Borrow an already-open status-runtime connection only after its fixed login,
 * role, search_path and row-security assertions have succeeded. The key-use
 * context never closes the borrowed connection. Calls through the two views
 * must remain serialized by their owning worker. */
int db2_management_status_key_ctx_borrow_hardened(db2_management_status_key_ctx_t *,
                                                  const db2_management_status_runtime_t *);
void db2_management_status_key_ctx_close(db2_management_status_key_ctx_t *);

typedef struct
{
   const char *use_id;
   const char *custody_key_id;
   const char *wire_key_id;
   int64_t version;
   const char *request_digest;
   const char *caller_issuer;
   const char *caller_serial_norm;
   const char *caller_fingerprint;
   const char *target_server_id;
   const char *target_mgmt_fingerprint;
   int64_t revocation_generation;
   const uint8_t *hwm_attestation;
   size_t hwm_attestation_len;
} db2_management_status_admission_t;

int db2_management_status_key_candidate(db2_management_status_key_ctx_t *,
                                        const char *custody_key_id, const char *wire_key_id,
                                        int64_t version, db2_vault_key_use_envelope_t *out);
/* 1=new admission, 0=exact terminal replay, typed negative error otherwise. */
int db2_management_status_key_admit(db2_management_status_key_ctx_t *,
                                    const db2_management_status_admission_t *,
                                    db2_vault_key_use_envelope_t *out);

/* Begin a transaction which holds the durable shared seal barrier until end. */
int db2_management_status_key_guard_begin(db2_management_status_key_ctx_t *,
                                          int64_t admitted_epoch);
int db2_management_status_key_guard_end(db2_management_status_key_ctx_t *, int commit);
int db2_management_status_key_startup_begin(db2_management_status_key_ctx_t *, int64_t *epoch,
                                            int *sealed);
int db2_management_status_key_startup_end(db2_management_status_key_ctx_t *, int commit);

#endif
