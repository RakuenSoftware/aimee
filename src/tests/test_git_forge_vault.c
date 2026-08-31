/* test_git_forge_vault.c — WP-B: a webchat user's git credential round-trips
 * through the sealed per-principal vault and is readable AUTONOMOUSLY (server
 * wrap) after the user's session KEK expires — while staying isolated to the
 * owning webuser. Mirrors the vault_service harness (tmp AIMEE_HOME). */
#include "modules/git/git_forge_vault.h"
#include "vault_kek_cache.h"
#include "vault_service.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Injected config readers for git_identity_resolve_with. */
static int reader_none(const char *key, char *out, size_t out_len, void *ud)
{
   (void)key;
   (void)ud;
   if (out && out_len)
      out[0] = '\0';
   return 0;
}

static int reader_both(const char *key, char *out, size_t out_len, void *ud)
{
   (void)ud;
   snprintf(out, out_len, "%s",
            strcmp(key, "user.name") == 0 ? "Runner Operator" : "runner@example.test");
   return 1;
}

static int reader_name_only(const char *key, char *out, size_t out_len, void *ud)
{
   (void)ud;
   if (strcmp(key, "user.name") != 0)
   {
      out[0] = '\0';
      return 0;
   }
   snprintf(out, out_len, "Runner Operator");
   return 1;
}

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
   assert(vault_service_set_server(GIT_FORGE_VAULT_AGENT, GIT_FORGE_TOKEN_CRED, "ghp_alicePAT") ==
          VAULT_OK);
   const char *akey = "-----BEGIN OPENSSH PRIVATE KEY-----\nenvKEY\n-----END-----";
   assert(vault_service_set_server(GIT_FORGE_VAULT_AGENT, GIT_FORGE_SSHKEY_CRED, akey) == VAULT_OK);

   /* Readable while unlocked. */
   assert(git_forge_vault_token(alice, out, sizeof(out)) == 1);
   assert(strcmp(out, "ghp_alicePAT") == 0);

   /* Simulate a background/idle git op: drop every cached user KEK so the user
    * vault is LOCKED — the server wrap must STILL read both creds. */
   vault_kek_cache_clear();
   assert(vault_service_get(alice, GIT_FORGE_VAULT_AGENT, GIT_FORGE_TOKEN_CRED, out, sizeof(out),
                            T0) != VAULT_OK);                   /* no per-user entry... */
   assert(git_forge_vault_token(alice, out, sizeof(out)) == 1); /* ...server wrap still reads */
   assert(strcmp(out, "ghp_alicePAT") == 0);
   assert(git_forge_vault_sshkey(alice, out, sizeof(out)) == 1);
   assert(strcmp(out, akey) == 0);

   /* aimee-server is single-tenant: there is ONE git credential for the
    * environment, and every actor resolves it. The PAM actor authenticates and
    * is audited; it does not select a credential namespace. (Per-principal
    * isolation belongs on aimee-kb, which serves many; see kb_scope.) */
   vault_kek_cache_clear();
   assert(git_forge_vault_token(bob, out, sizeof(out)) == 1);
   assert(strcmp(out, "ghp_alicePAT") == 0);
   assert(git_forge_vault_token(alice, out, sizeof(out)) == 1);
   assert(strcmp(out, "ghp_alicePAT") == 0);

   /* SERVER-SEALED INTAKE (the /v1/git/sshkey write path): a webuser who has
    * NEVER unlocked can seal an SSH key with the server KEK alone — no cached user
    * KEK, no 423 — and the server reads it back autonomously. */
   const char *carol = "webuser:carol";
   const char *ckey = "-----BEGIN OPENSSH PRIVATE KEY-----\ncarolKEY\n-----END-----";
   vault_kek_cache_clear(); /* prove no user KEK is cached for carol */
   /* RED: the per-user-KEK intake is LOCKED for a never-unlocked actor (the 423
    * this removes). GREEN: the server-sealed intake just works. */
   assert(vault_service_set(carol, GIT_FORGE_VAULT_AGENT, GIT_FORGE_SSHKEY_CRED, ckey, T0) ==
          VAULT_ERR_LOCKED);
   assert(vault_service_set_server(GIT_FORGE_VAULT_AGENT, GIT_FORGE_SSHKEY_CRED, ckey) == VAULT_OK);
   vault_kek_cache_clear();
   assert(git_forge_vault_sshkey(carol, out, sizeof(out)) == 1);
   assert(strcmp(out, ckey) == 0);
   /* The user-KEK path cannot read the environment entry: it is sealed under the
    * server master KEK, so a stale per-user vault is not a second way in. */
   assert(vault_service_get(carol, GIT_FORGE_VAULT_AGENT, GIT_FORGE_SSHKEY_CRED, out, sizeof(out),
                            T0) != VAULT_OK);
   /* One environment, one key: sealing again replaced it, and every actor now
    * reads the new one rather than a per-user copy. */
   assert(git_forge_vault_sshkey(alice, out, sizeof(out)) == 1);
   assert(strcmp(out, ckey) == 0);
   /* Delete round-trips with no unlock too (DELETE /v1/git/sshkey). */
   assert(vault_service_delete(VAULT_SERVER_PRINCIPAL, GIT_FORGE_VAULT_AGENT,
                               GIT_FORGE_SSHKEY_CRED) == VAULT_OK);
   assert(git_forge_vault_sshkey(carol, out, sizeof(out)) == 0);
   assert(out[0] == '\0');

   /* --- git_identity_resolve: the checkout's own identity is a valid source ---
    * The sealed identity is an install-time step. Requiring it as the ONLY source
    * stranded any running agent that reached a commit with a clean vault: it could
    * only stop and ask a human to re-run installation. Fall back to the identity
    * the operator already configured for the repository. */
   /* Needs a real git binary; the injected-reader cases below cover the same
    * resolution policy without one, so skip rather than fail where git is
    * absent (e.g. a minimal build image). */
   if (system("git --version >/dev/null 2>&1") != 0)
      printf("git_forge_vault: no git binary — skipping the on-disk config cases\n");
   else
   {
      char repo[320];
      snprintf(repo, sizeof(repo), "%s/repo", home);
      char cmd[900];
      snprintf(cmd, sizeof(cmd), "mkdir -p %s && git -C %s init -q", repo, repo);
      assert(system(cmd) == 0);
      /* Point HOME at the sandbox: the fallback also consults the operator's
       * ordinary git identity, so a real ~/.gitconfig on the machine running
       * these tests would otherwise satisfy the "nothing configured" cases and
       * make the result depend on who runs them. */
      setenv("HOME", home, 1);

      char name[256], email[256];
      /* Nothing sealed, nothing configured on the checkout: still refuses, and
       * still refuses to invent a persona. */
      assert(git_identity_resolve(repo, name, sizeof(name), email, sizeof(email)) == 0);
      assert(name[0] == '\0' && email[0] == '\0');

      /* The operator's normal global identity is valid too. This is the common
       * workstation setup and must not require duplicating identity into every
       * Aimee-managed checkout. */
      assert(system("git config --global user.name 'Global Operator' && "
                    "git config --global user.email 'global@example.test'") == 0);
      setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
      setenv("GIT_CONFIG_SYSTEM", "/dev/null", 1);
      setenv("GIT_CONFIG_GLOBAL", "/dev/null", 1);
      assert(git_identity_resolve(repo, name, sizeof(name), email, sizeof(email)) == 1);
      assert(strcmp(name, "Global Operator") == 0);
      assert(strcmp(email, "global@example.test") == 0);
      unsetenv("GIT_CONFIG_NOSYSTEM");
      unsetenv("GIT_CONFIG_SYSTEM");
      unsetenv("GIT_CONFIG_GLOBAL");
      assert(system("git config --global --unset-all user.name && "
                    "git config --global --unset-all user.email") == 0);

      /* Half an identity is not an identity — same rule as the vault path. */
      snprintf(cmd, sizeof(cmd), "git -C %s config user.name 'Repo Operator'", repo);
      assert(system(cmd) == 0);
      assert(git_identity_resolve(repo, name, sizeof(name), email, sizeof(email)) == 0);
      assert(name[0] == '\0' && email[0] == '\0');

      /* Both configured: resolves, and the work proceeds without a vault seal. */
      snprintf(cmd, sizeof(cmd), "git -C %s config user.email 'repo@example.test'", repo);
      assert(system(cmd) == 0);
      assert(git_identity_resolve(repo, name, sizeof(name), email, sizeof(email)) == 1);
      assert(strcmp(name, "Repo Operator") == 0);
      assert(strcmp(email, "repo@example.test") == 0);

      /* A sealed identity still WINS over the checkout's — the install-time
       * identity is the authoritative one where it exists. */
      assert(vault_service_set_server(GIT_FORGE_VAULT_AGENT, GIT_AUTHOR_NAME_CRED,
                                      "Sealed Operator") == VAULT_OK);
      assert(vault_service_set_server(GIT_FORGE_VAULT_AGENT, GIT_AUTHOR_EMAIL_CRED,
                                      "sealed@example.test") == VAULT_OK);
      assert(git_identity_resolve(repo, name, sizeof(name), email, sizeof(email)) == 1);
      assert(strcmp(name, "Sealed Operator") == 0);
      assert(strcmp(email, "sealed@example.test") == 0);
   }

   /* --- git_identity_resolve_with: the CALLER supplies the lookup ---
    * The MCP git tools do not run in the server process's working directory --
    * a workspace provider decides where git runs -- so an in-process lookup
    * consults a directory that is not the checkout and silently finds nothing.
    * Those callers inject a reader that routes through the same runner they
    * commit with. */
   {
      char name[256], email[256];
      /* Unseal first, or the sealed identity above would answer instead of the
       * injected reader and these would prove nothing. */
      assert(vault_service_delete(VAULT_SERVER_PRINCIPAL, GIT_FORGE_VAULT_AGENT,
                                  GIT_AUTHOR_NAME_CRED) == VAULT_OK);
      assert(vault_service_delete(VAULT_SERVER_PRINCIPAL, GIT_FORGE_VAULT_AGENT,
                                  GIT_AUTHOR_EMAIL_CRED) == VAULT_OK);

      /* A reader that finds nothing: still refuses, still no invented author. */
      assert(git_identity_resolve_with(NULL, reader_none, NULL, name, sizeof(name), email,
                                       sizeof(email)) == 0);
      assert(name[0] == '\0' && email[0] == '\0');

      /* A reader that answers both: resolves from the caller's context. */
      assert(git_identity_resolve_with(NULL, reader_both, NULL, name, sizeof(name), email,
                                       sizeof(email)) == 1);
      assert(strcmp(name, "Runner Operator") == 0);
      assert(strcmp(email, "runner@example.test") == 0);

      /* A reader that answers only the name: half an identity is not one. */
      assert(git_identity_resolve_with(NULL, reader_name_only, NULL, name, sizeof(name), email,
                                       sizeof(email)) == 0);
      assert(name[0] == '\0' && email[0] == '\0');

      /* No reader at all is a refusal, not a crash. */
      assert(git_identity_resolve_with(NULL, NULL, NULL, name, sizeof(name), email,
                                       sizeof(email)) == 0);

      /* --- identity is the ENVIRONMENT's, not the actor's ---
       * aimee-server is single-tenant, so a commit is authored by the configured
       * environment identity whoever triggered it. A per-actor identity sealed in
       * the vault is NOT consulted: it would let the actor rewrite authorship,
       * and there is only one actor to distinguish anyway. The actor is carried
       * for audit, and the resolver falls through to its ordinary tiers. */
      assert(vault_service_set_server_wrap(alice, GIT_FORGE_VAULT_AGENT, GIT_AUTHOR_NAME_CRED,
                                           "Alice A") == VAULT_OK);
      assert(vault_service_set_server_wrap(alice, GIT_FORGE_VAULT_AGENT, GIT_AUTHOR_EMAIL_CRED,
                                           "alice@example.test") == VAULT_OK);
      vault_kek_cache_clear(); /* autonomous read: no cached user KEK */
      assert(git_identity_resolve_with(alice, reader_both, NULL, name, sizeof(name), email,
                                       sizeof(email)) == 1);
      assert(strcmp(name, "Runner Operator") == 0);
      assert(strcmp(email, "runner@example.test") == 0);

      /* Every actor resolves that same identity — including one with nothing of
       * its own, which is now the only case there is. */
      assert(git_identity_resolve_with(bob, reader_both, NULL, name, sizeof(name), email,
                                       sizeof(email)) == 1);
      assert(strcmp(name, "Runner Operator") == 0);
      assert(strcmp(email, "runner@example.test") == 0);

      /* With nothing anywhere, it still refuses rather than inventing one. */
      assert(git_identity_resolve_with(bob, reader_none, NULL, name, sizeof(name), email,
                                       sizeof(email)) == 0);
      assert(name[0] == '\0' && email[0] == '\0');
   }

   char clean[320];
   snprintf(clean, sizeof(clean), "rm -rf %s", home);
   assert(system(clean) == 0);
   printf("git_forge_vault: all tests passed\n");
   return 0;
}
