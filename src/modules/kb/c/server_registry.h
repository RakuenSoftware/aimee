#ifndef KB_STORE_SERVER_REGISTRY_H
#define KB_STORE_SERVER_REGISTRY_H
#include <stddef.h>
#include <stdint.h>
typedef struct
{
   char server_id[128], cert_cn[256], mgmt_cert_cn[256], endpoint[512], status[32], health[128],
       version[64];
   int64_t team_id;
} kb_store_server_row_t;
typedef struct
{
   const char *operation, *server_id, *endpoint, *client_cn, *management_cn;
   const char *client_csr_digest, *management_csr_digest;
   int64_t team_id;
   int ttl_seconds;
} kb_store_server_pending_t;
typedef struct
{
   const char *issuer, *serial_norm, *fingerprint;
} kb_store_server_cert_identity_t;
typedef struct
{
   char server_id[128], endpoint[512], status[32];
   char management_issuer[601], management_serial_norm[129], management_fingerprint[65];
   char enrollment_state[32], revoked_at[64];
   int64_t revocation_generation;
} kb_store_server_snapshot_t;
int kb_store_server_registry_list(int64_t, kb_store_server_row_t *, int);
int kb_store_server_registry_get(int64_t, const char *, kb_store_server_row_t *);
int kb_store_server_registry_pending(const kb_store_server_pending_t *, char *, size_t);
int kb_store_server_registry_finalize(const char *, const char *, const char *,
                                      const kb_store_server_cert_identity_t *,
                                      const kb_store_server_cert_identity_t *, char *, size_t);
int kb_store_server_registry_heartbeat(const char *, const char *, const char *, const char *,
                                       const char *, const char *);
/* 1 when the named active server/team is bound to this exact active client
 * certificate, 0 on authoritative denial, -1 on database/protocol failure. */
int kb_store_server_registry_client_match(const char *, int64_t, const char *, const char *,
                                          const char *);
/* 0 row loaded, 1 authoritative absence, -1 database/protocol failure. */
int kb_store_server_registry_snapshot(int64_t, const char *, kb_store_server_snapshot_t *);
int kb_store_management_status_lookup(const char *, const char *, const char *, const char *,
                                      const char *, int64_t *, char *, size_t);
#endif
