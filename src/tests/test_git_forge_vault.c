/* test_git_forge_vault.c — WP-B: a webchat user's git credential round-trips
 * through the sealed per-principal vault and is readable AUTONOMOUSLY (server
 * wrap) after the user's session KEK expires — while staying isolated to the
 * owning webuser. Mirrors the vault_service harness (tmp AIMEE_HOME). */
#include "git_forge_vault.h"
#include "vault_kek_cache.h"
#include "vault_service.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
   char home[256];
   snprintf(home, sizeof(home), "/tmp/aimee-gitforge-test-%d", (int)getpid());
   char mk[320];
   snprintf(mk, sizeof(mk), "rm -rf %s && mkdir -p %s", home, home);
   assert(system(mk) == 0);
   setenv("AIMEE_HOME", home, 1);
   vault_kek_cache_clear();

   const long T0 = 100000;
   const char *alice = "webuser:alice";
   const char *bob = "webuser:bob";
   char out[4096];

   /* Nothing stored yet -> 0 (caller falls back to ambient creds), out empty. */
   assert(git_forge_vault_token(alice, out, sizeof(out)) == 0);
   assert(out[0] == '\0');

   /* Intake path: unlock alice's webuser vault (webchat-trusted) + store a PAT
    * and an SSH key under the git convention (this is what /v1/vault/set does). */
   const uint8_t apw[] = "alice-login-pw";
   assert(vault_service_unlock_password(alice, ATTEST_WEBCHAT_TRUSTED, apw, sizeof(apw) - 1, T0) ==
          VAULT_OK);
   assert(vault_service_set(alice, GIT_FORGE_VAULT_AGENT, GIT_FORGE_TOKEN_CRED, "ghp_alicePAT",
                            T0) == VAULT_OK);
   const char *akey = "-----BEGIN OPENSSH PRIVATE KEY-----\naliceKEY\n-----END-----";
   assert(vault_service_set(alice, GIT_FORGE_VAULT_AGENT, GIT_FORGE_SSHKEY_CRED, akey, T0) ==
          VAULT_OK);

   /* Readable while unlocked. */
   assert(git_forge_vault_token(alice, out, sizeof(out)) == 1);
   assert(strcmp(out, "ghp_alicePAT") == 0);

   /* Simulate a background/idle git op: drop every cached user KEK so the user
    * vault is LOCKED — the server wrap must STILL read both creds. */
   vault_kek_cache_clear();
   assert(vault_service_get(alice, GIT_FORGE_VAULT_AGENT, GIT_FORGE_TOKEN_CRED, out, sizeof(out),
                            T0) == VAULT_ERR_LOCKED);           /* user path is locked... */
   assert(git_forge_vault_token(alice, out, sizeof(out)) == 1); /* ...server wrap still reads */
   assert(strcmp(out, "ghp_alicePAT") == 0);
   assert(git_forge_vault_sshkey(alice, out, sizeof(out)) == 1);
   assert(strcmp(out, akey) == 0);

   /* Cross-principal isolation: bob has no git token and cannot read alice's. */
   assert(git_forge_vault_token(bob, out, sizeof(out)) == 0);
   assert(out[0] == '\0');
   const uint8_t bpw[] = "bob-login-pw";
   assert(vault_service_unlock_password(bob, ATTEST_WEBCHAT_TRUSTED, bpw, sizeof(bpw) - 1, T0) ==
          VAULT_OK);
   assert(vault_service_set(bob, GIT_FORGE_VAULT_AGENT, GIT_FORGE_TOKEN_CRED, "ghp_bobPAT", T0) ==
          VAULT_OK);
   vault_kek_cache_clear();
   assert(git_forge_vault_token(bob, out, sizeof(out)) == 1);
   assert(strcmp(out, "ghp_bobPAT") == 0);
   /* alice still resolves to alice's token, never bob's. */
   assert(git_forge_vault_token(alice, out, sizeof(out)) == 1);
   assert(strcmp(out, "ghp_alicePAT") == 0);

   /* An empty/un-attested principal is NO_ENTRY (0), never a leak. */
   assert(git_forge_vault_token("", out, sizeof(out)) == 0);
   assert(out[0] == '\0');

   /* SERVER-SEALED INTAKE (the /v1/git/sshkey write path): a webuser who has
    * NEVER unlocked can seal an SSH key with the server KEK alone — no cached user
    * KEK, no 423 — and the server reads it back autonomously. */
   const char *carol = "webuser:carol";
   const char *ckey = "-----BEGIN OPENSSH PRIVATE KEY-----\ncarolKEY\n-----END-----";
   vault_kek_cache_clear(); /* prove no user KEK is cached for carol */
   /* RED: the old per-user-KEK intake is LOCKED for a never-unlocked user (the 423
    * this change removes). GREEN: the server-sealed intake below just works. */
   assert(vault_service_set(carol, GIT_FORGE_VAULT_AGENT, GIT_FORGE_SSHKEY_CRED, ckey, T0) ==
          VAULT_ERR_LOCKED);
   assert(vault_service_set_server_wrap(carol, GIT_FORGE_VAULT_AGENT, GIT_FORGE_SSHKEY_CRED,
                                        ckey) == VAULT_OK);
   vault_kek_cache_clear();
   assert(git_forge_vault_sshkey(carol, out, sizeof(out)) == 1);
   assert(strcmp(out, ckey) == 0);
   /* The user-KEK path can't read a server-only entry carol never unlocked. */
   assert(vault_service_get(carol, GIT_FORGE_VAULT_AGENT, GIT_FORGE_SSHKEY_CRED, out, sizeof(out),
                            T0) != VAULT_OK);
   /* Isolation holds: alice's own key is unaffected; carol never sees it. */
   assert(git_forge_vault_sshkey(alice, out, sizeof(out)) == 1);
   assert(strcmp(out, akey) == 0);
   /* Delete round-trips with no unlock too (DELETE /v1/git/sshkey). */
   assert(vault_service_delete(carol, GIT_FORGE_VAULT_AGENT, GIT_FORGE_SSHKEY_CRED) == VAULT_OK);
   assert(git_forge_vault_sshkey(carol, out, sizeof(out)) == 0);
   assert(out[0] == '\0');

   char clean[320];
   snprintf(clean, sizeof(clean), "rm -rf %s", home);
   assert(system(clean) == 0);
   printf("git_forge_vault: all tests passed\n");
   return 0;
}
