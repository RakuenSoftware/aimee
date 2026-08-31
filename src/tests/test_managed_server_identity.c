#include "kb/managed_server_identity.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
   kb_pki_ca_t ca;
   assert(kb_pki_ca_generate(&ca) == 0);

   kb_managed_server_identity_t identity;
   assert(kb_managed_server_identity_generate(&ca, "aimee-kb", 8745, "https://aimee-server:8743", 7,
                                              &identity) == 0);
   assert(strcmp(identity.state, "pending") == 0);
   assert(strncmp(identity.server_id, "managed-", 8) == 0);
   assert(strcmp(identity.client_csr_digest, identity.management_csr_digest) != 0);
   assert(kb_managed_server_identity_validate(&identity) == 1);

   assert(kb_managed_server_identity_issue(&ca, &identity) == 0);
   assert(strcmp(identity.state, "issued") == 0);
   assert(kb_managed_server_identity_validate(&identity) == 1);

   kb_managed_server_identity_t reused = identity;
   snprintf(reused.management_cert, sizeof(reused.management_cert), "%s", reused.client_cert);
   snprintf(reused.management_key, sizeof(reused.management_key), "%s", reused.client_key);
   assert(kb_managed_server_identity_validate(&reused) == 0);
   kb_managed_server_identity_clear(&reused);

   char path[256];
   snprintf(path, sizeof(path), "/tmp/aimee-managed-server-identity-%ld.json", (long)getpid());
   unlink(path);
   assert(kb_managed_server_identity_save(path, geteuid(), &identity) == 0);

   struct stat st;
   assert(stat(path, &st) == 0);
   assert(st.st_uid == geteuid());
   assert((st.st_mode & 0777) == 0600);

   kb_managed_server_identity_t loaded;
   assert(kb_managed_server_identity_load(path, geteuid(), &loaded) == 0);
   assert(strcmp(loaded.server_id, identity.server_id) == 0);
   assert(strcmp(loaded.operation, identity.operation) == 0);
   assert(strcmp(loaded.client_cert, identity.client_cert) == 0);
   assert(strcmp(loaded.management_cert, identity.management_cert) == 0);

   snprintf(loaded.state, sizeof(loaded.state), "ready");
   assert(kb_managed_server_identity_save(path, geteuid(), &loaded) == 0);
   kb_managed_server_identity_clear(&identity);
   assert(kb_managed_server_identity_load(path, geteuid(), &identity) == 0);
   assert(strcmp(identity.state, "ready") == 0);

   identity.client_csr_digest[0] = identity.client_csr_digest[0] == '0' ? '1' : '0';
   assert(kb_managed_server_identity_validate(&identity) == 0);
   assert(kb_managed_server_identity_save(path, geteuid(), &identity) == -1);

   kb_managed_server_identity_clear(&loaded);
   kb_managed_server_identity_clear(&identity);
   assert(unlink(path) == 0);
   puts("managed server identity lifecycle: ok");
   return 0;
}
