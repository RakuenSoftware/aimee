#include "db1.h"
#include "server_mgmt_status.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static server_tls_peer_cert_t peer(char c)
{
   server_tls_peer_cert_t p = {0};
   snprintf(p.cn, sizeof(p.cn), "p5-kb-management");
   snprintf(p.issuer, sizeof(p.issuer), "/CN=test-ca");
   snprintf(p.serial_norm, sizeof(p.serial_norm), "01ab");
   memset(p.fingerprint, c, 64);
   memset(p.channel_binding, c + 1, 64);
   p.fingerprint[64] = p.channel_binding[64] = '\0';
   p.management_profile = 1;
   return p;
}

static kb_mgmt_status_t issue(const server_tls_peer_cert_t *p, uint64_t now)
{
   kb_mgmt_status_t s = {0};
   uint64_t expiry = 0;
   assert(server_mgmt_nonce_issue(p, "server-1", now, s.nonce, &expiry) == 0);
   assert(expiry == now + 15);
   s.revocation_generation = 4;
   return s;
}

int main(void)
{
   char path[256];
   snprintf(path, sizeof path, "%s/aimee-mgmt-status-XXXXXX", platform_tmpdir());
   int fd = mkstemp(path);
   assert(fd >= 0);
   close(fd);
   assert(db1_init(path) == 0 && server_mgmt_status_init() == 0);
   server_tls_peer_cert_t p = peer('a'), other = peer('c');

   kb_mgmt_status_t s = issue(&p, 100);
   assert(server_mgmt_nonce_consume(&s, &other, "server-1", 101, 1) == SERVER_MGMT_NONCE_MISMATCH);
   assert(server_mgmt_nonce_consume(&s, &p, "server-1", 101, 1) == SERVER_MGMT_NONCE_NOT_FOUND);

   s = issue(&p, 100);
   assert(server_mgmt_nonce_consume(&s, &p, "server-1", 101, 0) == SERVER_MGMT_NONCE_INVALID);
   assert(server_mgmt_nonce_consume(&s, &p, "server-1", 101, 1) == SERVER_MGMT_NONCE_NOT_FOUND);

   s = issue(&p, 100);
   assert(server_mgmt_nonce_consume(&s, &p, "server-1", 116, 1) == SERVER_MGMT_NONCE_EXPIRED);

   s = issue(&p, 100);
   assert(server_mgmt_nonce_consume(&s, &p, "server-1", 101, 1) == SERVER_MGMT_NONCE_OK);
   uint64_t hwm = 0;
   assert(server_mgmt_status_hwm(&hwm) == 0 && hwm == 4);
   s = issue(&p, 100);
   s.revocation_generation = 3;
   assert(server_mgmt_nonce_consume(&s, &p, "server-1", 101, 1) == SERVER_MGMT_NONCE_ROLLBACK);

   memset(&s, 0, sizeof(s));
   uint64_t action_expiry = 0;
   assert(server_mgmt_nonce_issue_purpose(&p, "server-1", "management.action.v1", 100, s.nonce,
                                          &action_expiry) == SERVER_MGMT_NONCE_OK);
   s.revocation_generation = 4;
   assert(server_mgmt_nonce_consume(&s, &p, "server-1", 101, 1) == SERVER_MGMT_NONCE_MISMATCH);
   assert(server_mgmt_nonce_consume_purpose(&s, &p, "server-1", "management.action.v1", 101, 1) ==
          SERVER_MGMT_NONCE_NOT_FOUND);
   assert(server_mgmt_nonce_issue_purpose(&p, "server-1", "management.action.v1", 100, s.nonce,
                                          &action_expiry) == SERVER_MGMT_NONCE_OK);
   assert(server_mgmt_nonce_consume_purpose(&s, &p, "server-1", "management.action.v1", 101, 1) ==
          SERVER_MGMT_NONCE_OK);

   memset(&s, 0, sizeof(s));
   assert(server_mgmt_nonce_issue_purpose(&p, "server-1", "management.read.v1", 200, s.nonce,
                                          &action_expiry) == SERVER_MGMT_NONCE_OK);
   assert(action_expiry == 215);
   s.revocation_generation = 4;
   assert(server_mgmt_nonce_consume_purpose(&s, &p, "server-1", "management.action.v1", 201, 1) ==
          SERVER_MGMT_NONCE_MISMATCH);
   assert(server_mgmt_nonce_consume_purpose(&s, &p, "server-1", "management.read.v1", 201, 1) ==
          SERVER_MGMT_NONCE_NOT_FOUND);

   memset(&s, 0, sizeof(s));
   assert(server_mgmt_nonce_issue_purpose(&p, "server-1", "management.read.config.v1", 200, s.nonce,
                                          &action_expiry) == SERVER_MGMT_NONCE_OK);
   s.revocation_generation = 4;
   assert(server_mgmt_nonce_consume_purpose(&s, &p, "server-1", "management.read.v1", 201, 1) ==
          SERVER_MGMT_NONCE_MISMATCH);
   assert(server_mgmt_nonce_consume_purpose(&s, &p, "server-1", "management.read.config.v1", 201,
                                            1) == SERVER_MGMT_NONCE_NOT_FOUND);
   assert(server_mgmt_nonce_issue_purpose(&p, "server-1", "management.read.config.v1", 200, s.nonce,
                                          &action_expiry) == SERVER_MGMT_NONCE_OK);
   assert(server_mgmt_nonce_consume_purpose(&s, &p, "server-1", "management.read.config.v1", 201,
                                            1) == SERVER_MGMT_NONCE_OK);

   assert(server_mgmt_nonce_issue_purpose(&p, "server-1", "management.read.v1", 200, s.nonce,
                                          &action_expiry) == SERVER_MGMT_NONCE_OK);
   assert(server_mgmt_nonce_consume_purpose(&s, &p, "server-1", "management.read.v1", 201, 1) ==
          SERVER_MGMT_NONCE_OK);
   assert(server_mgmt_nonce_consume_purpose(&s, &p, "server-1", "management.read.v1", 201, 1) ==
          SERVER_MGMT_NONCE_NOT_FOUND);

   s = issue(&p, 100);
   db1_shutdown();
   assert(db1_init(path) == 0 && server_mgmt_status_init() == 0);
   assert(server_mgmt_status_hwm(&hwm) == 0 && hwm == 4);
   assert(server_mgmt_nonce_consume(&s, &p, "server-1", 101, 1) == SERVER_MGMT_NONCE_NOT_FOUND);
   db1_shutdown();
   unlink(path);
   puts("server_mgmt_status: ok");
   return 0;
}
