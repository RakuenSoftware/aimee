#include "db1.h"
#include "remote_client_grant.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
   static const char hash_a[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
   static const char hash_b[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
   static const char serial_a[] = "01A2B3C4";
   db1_remote_client_grant_t grant;

   assert(db1_init(":memory:") == 0);
   assert(db1_remote_client_claim("webuser:alice", hash_a, 100, &grant) ==
          DB1_REMOTE_CLIENT_CLAIM_NEW);
   assert(strcmp(grant.principal, "webuser:alice") == 0);
   assert(strcmp(grant.bearer_sha256, hash_a) == 0);
   assert(grant.tier == 2 && !grant.cert_serial[0]);

   /* Refresh/retry is idempotent and cannot replace the pending bearer. */
   memset(&grant, 0, sizeof(grant));
   assert(db1_remote_client_claim("webuser:alice", hash_b, 101, &grant) ==
          DB1_REMOTE_CLIENT_CLAIM_UNBOUND);
   assert(strcmp(grant.bearer_sha256, hash_a) == 0);

   /* The first authenticated wizard user is immutable. */
   assert(db1_remote_client_claim("webuser:bob", hash_b, 102, &grant) ==
          DB1_REMOTE_CLIENT_CLAIM_OWNED_BY_OTHER);

   char principal[256];
   assert(db1_remote_client_tier(serial_a, principal, sizeof(principal)) == 0);
   assert(db1_remote_client_bind(hash_b, serial_a, 103) == 0); /* unrelated bearer */
   assert(db1_remote_client_bind(hash_a, serial_a, 103) == 1);
   assert(db1_remote_client_bind(hash_a, serial_a, 104) == 1); /* idempotent */
   assert(db1_remote_client_bind(hash_a, "DEADBEEF", 105) == -2);
   assert(db1_remote_client_tier(serial_a, principal, sizeof(principal)) == 2);
   assert(strcmp(principal, "webuser:alice") == 0);

   memset(&grant, 0, sizeof(grant));
   assert(db1_remote_client_claim("webuser:alice", hash_b, 106, &grant) ==
          DB1_REMOTE_CLIENT_CLAIM_BOUND);
   assert(strcmp(grant.cert_serial, serial_a) == 0);

   /* Abandon never removes an activated grant. */
   assert(db1_remote_client_abandon(hash_a) == 0);
   assert(db1_remote_client_tier(serial_a, principal, sizeof(principal)) == 2);

   db1_shutdown();
   puts("remote_client_grant: OK");
   return 0;
}
