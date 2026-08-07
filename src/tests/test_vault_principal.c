/* test_vault_principal.c: WP-C.0 — exhaustive characterization of the attested
 * vault-principal resolver. This is the single place "who owns this vault" is
 * decided, and the entire credential vault (WP-C.1/C.2) keys on it, so every
 * branch — including the fail-closed ones (uid 0, un-attested, spoofed webuser)
 * — is pinned here before any crypto consumes the principal. */
#include "vault_principal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static attested_transport_t resolve(int is_tcp, long uid, const char *webuser, int token_ok,
                                    char *out)
{
   out[0] = '\xff'; /* poison: prove the resolver writes/clears it */
   return vault_principal_resolve(is_tcp, 0 /*is_tls*/, uid, webuser, token_ok, NULL, out,
                                  VAULT_PRINCIPAL_MAX);
}

/* A native-TLS+bearer conn: attested for server-principal writes, no per-user
 * principal (server-principal vault). A valid webuser assertion still wins; a
 * tokenless webuser is NOT promoted to server authority. */
static void test_tls_bearer_attested_no_principal(void)
{
   char p[VAULT_PRINCIPAL_MAX];
   /* TLS over the network, no uid/webuser -> ATTEST_TLS_BEARER, empty principal. */
   assert(vault_principal_resolve(1, 1, -1, NULL, 0, NULL, p, sizeof(p)) == ATTEST_TLS_BEARER);
   assert(p[0] == '\0');
   /* A spoofed/tokenless webuser over TLS is REFUSED server authority: it is NOT
    * promoted to TLS_BEARER, it falls back to the raw transport (TCP_BEARER), with
    * no vault principal. (A misconfigured webchat forwarding an end-user header
    * must never mint a server credential merely because the socket has TLS.) */
   assert(vault_principal_resolve(1, 1, -1, "alice", 0, NULL, p, sizeof(p)) == ATTEST_TCP_BEARER);
   assert(p[0] == '\0');
   /* A valid webuser assertion still wins (per-user webchat vault). */
   assert(vault_principal_resolve(1, 1, -1, "alice", 1, NULL, p, sizeof(p)) ==
          ATTEST_WEBCHAT_TRUSTED);
   assert(strcmp(p, "webuser:alice") == 0);
   /* Plaintext TCP (no TLS) is NOT attested for server writes. */
   assert(vault_principal_resolve(1, 0, -1, NULL, 0, NULL, p, sizeof(p)) == ATTEST_TCP_BEARER);
   /* A short output buffer fails closed even over TLS (no TLS_BEARER on a buffer
    * too small to hold a principal). */
   char small[4];
   assert(vault_principal_resolve(1, 1, -1, NULL, 0, NULL, small, sizeof(small)) == ATTEST_NONE);
   printf("  PASS: test_tls_bearer_attested_no_principal\n");
}

/* A kernel-attested UDS peer with uid > 0 owns a uid: vault. */
static void test_uds_peer_uid_gets_principal(void)
{
   char p[VAULT_PRINCIPAL_MAX];
   assert(resolve(0, 1000, NULL, 0, p) == ATTEST_UDS_PEERCRED);
   assert(strcmp(p, "uid:1000") == 0);

   assert(resolve(0, 4242, "", 0, p) == ATTEST_UDS_PEERCRED);
   assert(strcmp(p, "uid:4242") == 0);
   printf("  PASS: test_uds_peer_uid_gets_principal\n");
}

/* Two distinct uids resolve to two distinct principals (isolation foundation). */
static void test_distinct_uids_distinct_principals(void)
{
   char a[VAULT_PRINCIPAL_MAX], b[VAULT_PRINCIPAL_MAX];
   assert(resolve(0, 1000, NULL, 0, a) == ATTEST_UDS_PEERCRED);
   assert(resolve(0, 1001, NULL, 0, b) == ATTEST_UDS_PEERCRED);
   assert(strcmp(a, b) != 0);
   assert(strcmp(a, "uid:1000") == 0 && strcmp(b, "uid:1001") == 0);
   printf("  PASS: test_distinct_uids_distinct_principals\n");
}

/* uid 0 is a real kernel attestation and gets uid:0. The missed-hop sentinel is
 * -1, NOT 0 -- server_http_identity_capture initialises to -1 and overwrites only
 * on a successful peer-cred lookup -- and a zeroed conn is caught by ATTEST_NONE
 * being the enum's zero value, so it never reaches this branch as UDS_PEERCRED.
 * Excluding root left `vault set-server` refusing every root caller, the ordinary
 * case inside the server container, while offering "grant (unknown)" as the fix.
 *
 * An unknown uid still gets NO principal and still fails closed. */
static void test_uid_zero_named_unknown_refused(void)
{
   char p[VAULT_PRINCIPAL_MAX];
   assert(resolve(0, 0, NULL, 0, p) == ATTEST_UDS_PEERCRED);
   assert(strcmp(p, "uid:0") == 0);

   assert(resolve(0, -1, NULL, 0, p) == ATTEST_UDS_PEERCRED);
   assert(p[0] == '\0');
   printf("  PASS: test_uid_zero_named_unknown_refused\n");
}

/* Plain TCP is bearer-authorized but has no OS-user attestation -> no vault. */
static void test_tcp_gets_no_principal(void)
{
   char p[VAULT_PRINCIPAL_MAX];
   assert(resolve(1, -1, NULL, 0, p) == ATTEST_TCP_BEARER);
   assert(p[0] == '\0');
   /* Even a (nonsensical) peer_uid on TCP yields no principal. */
   assert(resolve(1, 1000, NULL, 0, p) == ATTEST_TCP_BEARER);
   assert(p[0] == '\0');
   printf("  PASS: test_tcp_gets_no_principal\n");
}

/* A webuser asserted over the root-owned UDS is honored as webuser:<name>,
 * regardless of the underlying transport (the webchat backend rides UDS). */
static void test_webuser_with_token_honored(void)
{
   char p[VAULT_PRINCIPAL_MAX];
   assert(resolve(0, 33, "alice", 1, p) == ATTEST_WEBCHAT_TRUSTED);
   assert(strcmp(p, "webuser:alice") == 0);
   /* Over TCP too (the dispatch path carries the bearer). */
   assert(resolve(1, -1, "bob", 1, p) == ATTEST_WEBCHAT_TRUSTED);
   assert(strcmp(p, "webuser:bob") == 0);
   printf("  PASS: test_webuser_with_token_honored\n");
}

/* A webuser asserted WITHOUT the valid token is a spoof: the assertion is
 * refused (empty principal) and the conn falls back to its transport class. */
static void test_webuser_without_token_refused(void)
{
   char p[VAULT_PRINCIPAL_MAX];
   /* Spoof over UDS: no webuser principal; transport stays UDS, but a uid:N is
    * also NOT granted (the webuser assertion suppresses the uid path). */
   assert(resolve(0, 1000, "mallory", 0, p) == ATTEST_UDS_PEERCRED);
   assert(p[0] == '\0');
   /* Spoof over TCP: refused, plain bearer transport. */
   assert(resolve(1, -1, "mallory", 0, p) == ATTEST_TCP_BEARER);
   assert(p[0] == '\0');
   printf("  PASS: test_webuser_without_token_refused\n");
}

/* A short output buffer is a hard, fail-closed error (never a truncated
 * principal that could alias another user). */
static void test_short_buffer_fails_closed(void)
{
   char small[8];
   small[0] = '\xff';
   assert(vault_principal_resolve(0, 0, 1000, NULL, 0, NULL, small, sizeof(small)) == ATTEST_NONE);
   assert(small[0] == '\0');
   /* NULL out is also safe. */
   assert(vault_principal_resolve(0, 0, 1000, NULL, 0, NULL, NULL, 0) == ATTEST_NONE);
   printf("  PASS: test_short_buffer_fails_closed\n");
}

/* mTLS: a verified client cert -> cert:<CN>; a spoofing/unsanitizable CN is
 * refused (fail-closed) but still classified MTLS so required-mode rejects it. */
static void test_mtls_client_identity(void)
{
   char out[VAULT_PRINCIPAL_MAX];

   /* CN sanitization: valid charset accepted; spoofing/garbage refused. */
   char san[VAULT_PRINCIPAL_MAX];
   assert(vault_principal_name_sanitize("ci-runner_1.dev", san, sizeof(san)) == 1);
   assert(strcmp(san, "ci-runner_1.dev") == 0);
   assert(vault_principal_name_sanitize("uid:0", san, sizeof(san)) == 0); /* ':' refused */
   assert(vault_principal_name_sanitize("webuser:bob", san, sizeof(san)) == 0);
   assert(vault_principal_name_sanitize("a/b", san, sizeof(san)) == 0);       /* path traversal */
   assert(vault_principal_name_sanitize("a b", san, sizeof(san)) == 0);       /* whitespace */
   assert(vault_principal_name_sanitize("bad\nname", san, sizeof(san)) == 0); /* newline */
   assert(vault_principal_name_sanitize("", san, sizeof(san)) == 0);
   assert(san[0] == '\0');

   /* A verified cert over TLS -> cert:<CN>, wins over a tokenless bearer/webuser. */
   assert(vault_principal_resolve(1, 1, -1, NULL, 0, "ci-runner-1", out, sizeof(out)) ==
          ATTEST_MTLS_CLIENT);
   assert(strcmp(out, "cert:ci-runner-1") == 0);
   /* Cert identity wins even when a (tokenless) webuser header is also present. */
   assert(vault_principal_resolve(1, 1, -1, "alice", 0, "ci-runner-1", out, sizeof(out)) ==
          ATTEST_MTLS_CLIENT);
   assert(strcmp(out, "cert:ci-runner-1") == 0);

   /* A spoofing CN: classified MTLS but NO principal (required-mode refuses it). */
   assert(vault_principal_resolve(1, 1, -1, NULL, 0, "uid:0", out, sizeof(out)) ==
          ATTEST_MTLS_CLIENT);
   assert(out[0] == '\0');

   /* A cert CN but no TLS (impossible in practice) is ignored -> bearer path. */
   assert(vault_principal_resolve(1, 0, -1, NULL, 0, "ci-runner-1", out, sizeof(out)) ==
          ATTEST_TCP_BEARER);
   assert(out[0] == '\0');
   printf("  PASS: test_mtls_client_identity\n");
}

/* The sanitize rule is ONE rule for every attacker-influenced name that becomes
 * part of a principal. The cert CN was sanitized and the webuser name was not,
 * for no stated reason; this asserts they now behave identically, case for case,
 * so the two cannot diverge again without a test failing. */
static void test_name_sanitize_is_one_rule(void)
{
   struct
   {
      const char *name;
      int ok;
      const char *why;
   } cases[] = {
       {"alice", 1, "an ordinary login name"},
       {"ci-runner_1.dev", 1, "dashes, underscores and dots"},
       {"A1", 1, "mixed case and digits"},
       {"uid:0", 0, "':' would embed the namespace separator"},
       {"webuser:bob", 0, "likewise a full principal form"},
       {"a/b", 0, "'/' is a path separator"},
       {"../etc/shadow", 0, "path traversal"},
       {"a b", 0, "whitespace"},
       {"bad\nname", 0, "a newline is a log-injection primitive"},
       {"bad\tname", 0, "a tab is a control character"},
       {"pipe|name", 0, "'|' is the vault AAD delimiter"},
       {"machine$", 0, "a Samba machine account is outside the set (documented)"},
       {"", 0, "empty"},
   };

   char san[VAULT_PRINCIPAL_MAX];
   char cert_out[VAULT_PRINCIPAL_MAX], web_out[VAULT_PRINCIPAL_MAX];
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
   {
      const char *n = cases[i].name;
      int expect = cases[i].ok;
      assert(vault_principal_name_sanitize(n, san, sizeof(san)) == expect);
      if (!expect)
         assert(san[0] == '\0');

      /* THE point: the cert path and the webuser path must reach the same verdict
       * for the same name. Each keeps its own transport classification either way,
       * so an unusable name costs the vault identity and nothing else. */
      attested_transport_t ct =
          vault_principal_resolve(1, 1, -1, NULL, 0, n[0] ? n : NULL, cert_out, sizeof(cert_out));
      attested_transport_t wt =
          vault_principal_resolve(1, 0, -1, n[0] ? n : NULL, 1, NULL, web_out, sizeof(web_out));

      if (n[0])
      {
         assert(ct == ATTEST_MTLS_CLIENT);
         assert(wt == ATTEST_WEBCHAT_TRUSTED);
      }
      if (expect)
      {
         char want_cert[VAULT_PRINCIPAL_MAX], want_web[VAULT_PRINCIPAL_MAX];
         snprintf(want_cert, sizeof(want_cert), "cert:%s", n);
         snprintf(want_web, sizeof(want_web), "webuser:%s", n);
         assert(strcmp(cert_out, want_cert) == 0);
         assert(strcmp(web_out, want_web) == 0);
      }
      else if (n[0])
      {
         /* Both refuse, and neither leaves a partial principal behind. */
         assert(cert_out[0] == '\0');
         assert(web_out[0] == '\0');
      }
   }

   /* A name at the length limit is accepted; one over it is refused rather than
    * truncated — two long names sharing a prefix must never collapse onto one
    * principal and share a vault. */
   char at_limit[VAULT_CERT_CN_MAX + 1];
   memset(at_limit, 'a', VAULT_CERT_CN_MAX);
   at_limit[VAULT_CERT_CN_MAX] = '\0';
   assert(vault_principal_name_sanitize(at_limit, san, sizeof(san)) == 1);
   char over_limit[VAULT_CERT_CN_MAX + 2];
   memset(over_limit, 'a', VAULT_CERT_CN_MAX + 1);
   over_limit[VAULT_CERT_CN_MAX + 1] = '\0';
   assert(vault_principal_name_sanitize(over_limit, san, sizeof(san)) == 0);

   /* Two distinct over-limit names both refused -> they cannot collide. */
   char other_over[VAULT_CERT_CN_MAX + 2];
   memset(other_over, 'b', VAULT_CERT_CN_MAX + 1);
   other_over[VAULT_CERT_CN_MAX + 1] = '\0';
   assert(vault_principal_resolve(1, 0, -1, over_limit, 1, NULL, web_out, sizeof(web_out)) ==
          ATTEST_WEBCHAT_TRUSTED);
   assert(web_out[0] == '\0');
   assert(vault_principal_resolve(1, 0, -1, other_over, 1, NULL, cert_out, sizeof(cert_out)) ==
          ATTEST_WEBCHAT_TRUSTED);
   assert(cert_out[0] == '\0');
   printf("  PASS: test_name_sanitize_is_one_rule\n");
}

int main(void)
{
   test_uds_peer_uid_gets_principal();
   test_distinct_uids_distinct_principals();
   test_uid_zero_named_unknown_refused();
   test_tcp_gets_no_principal();
   test_webuser_with_token_honored();
   test_webuser_without_token_refused();
   test_tls_bearer_attested_no_principal();
   test_short_buffer_fails_closed();
   test_mtls_client_identity();
   test_name_sanitize_is_one_rule();
   printf("vault_principal: all tests passed\n");
   return 0;
}
