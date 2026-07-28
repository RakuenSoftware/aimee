#ifndef AIMEE_MANAGED_SERVER_IDENTITY_H
#define AIMEE_MANAGED_SERVER_IDENTITY_H

#include "kb_pki.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct
{
   int version;
   char state[16];
   char host[256];
   int port;
   char endpoint[512];
   char server_id[128];
   int64_t team_id;
   char operation[33];
   char client_csr_digest[65];
   char management_csr_digest[65];
   char ca[KB_PKI_CERT_PEM_MAX];
   char client_csr[KB_PKI_CSR_PEM_MAX];
   char client_key[KB_PKI_KEY_PEM_MAX];
   char client_cert[KB_PKI_CERT_PEM_MAX];
   char management_csr[KB_PKI_CSR_PEM_MAX];
   char management_key[KB_PKI_KEY_PEM_MAX];
   char management_cert[KB_PKI_CERT_PEM_MAX];
} kb_managed_server_identity_t;

int kb_managed_server_identity_generate(const kb_pki_ca_t *ca, const char *host, int port,
                                        const char *endpoint, int64_t team_id,
                                        kb_managed_server_identity_t *out);
int kb_managed_server_identity_issue(const kb_pki_ca_t *ca, kb_managed_server_identity_t *identity);
int kb_managed_server_identity_validate(const kb_managed_server_identity_t *identity);
int kb_managed_server_identity_load(const char *path, uid_t expected_owner,
                                    kb_managed_server_identity_t *out);
int kb_managed_server_identity_save(const char *path, uid_t owner,
                                    const kb_managed_server_identity_t *identity);
void kb_managed_server_identity_clear(kb_managed_server_identity_t *identity);

#endif
