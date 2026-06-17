/* test_aimee_tls_clientcert.c: pin the client mTLS-material gate.
 *
 * aimee_tls_client_cert_eligible() decides whether <home>/tls/client.{crt,key}
 * is presented as the thin client's certificate. The security-relevant property
 * is FAIL CLOSED: a group/world-readable private key must NOT be presented (the
 * key is the client's identity material). These tests pin that contract without
 * needing OpenSSL or a network — the helper is pure (stat + path build). */
#include "aimee_tls.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_home[512];

static void write_file(const char *path, mode_t mode)
{
   FILE *f = fopen(path, "w");
   assert(f);
   fputs("x\n", f);
   fclose(f);
   assert(chmod(path, mode) == 0);
}

static void make_home(void)
{
   snprintf(g_home, sizeof(g_home), "/tmp/aimee-tlscc-%d", (int)getpid());
   char tls[600];
   mkdir(g_home, 0700);
   snprintf(tls, sizeof(tls), "%s/tls", g_home);
   mkdir(tls, 0700);
}

static void rm_material(void)
{
   char p[700];
   snprintf(p, sizeof(p), "%s/tls/client.crt", g_home);
   unlink(p);
   snprintf(p, sizeof(p), "%s/tls/client.key", g_home);
   unlink(p);
}

/* No files configured -> 0 (plain client TLS, no cert presented). */
static void test_absent_is_none(void)
{
   rm_material();
   char crt[600], key[600];
   int r = aimee_tls_client_cert_eligible(g_home, crt, sizeof(crt), key, sizeof(key));
   assert(r == 0);
}

/* Both files present, key 0600 -> 1 (present), and paths resolved correctly. */
static void test_present_when_0600(void)
{
   char crt[600], key[600], p[700];
   snprintf(p, sizeof(p), "%s/tls/client.crt", g_home);
   write_file(p, 0600);
   snprintf(p, sizeof(p), "%s/tls/client.key", g_home);
   write_file(p, 0600);
   int r = aimee_tls_client_cert_eligible(g_home, crt, sizeof(crt), key, sizeof(key));
   assert(r == 1);
   char want[700];
   snprintf(want, sizeof(want), "%s/tls/client.crt", g_home);
   assert(strcmp(crt, want) == 0);
   snprintf(want, sizeof(want), "%s/tls/client.key", g_home);
   assert(strcmp(key, want) == 0);
}

/* Key group/world-readable -> -1 (fail closed: never present a loose key). */
static void test_refuse_loose_key(void)
{
   char crt[600], key[600], p[700];
   snprintf(p, sizeof(p), "%s/tls/client.crt", g_home);
   write_file(p, 0644);
   snprintf(p, sizeof(p), "%s/tls/client.key", g_home);
   write_file(p, 0644); /* world-readable private key */
   int r = aimee_tls_client_cert_eligible(g_home, crt, sizeof(crt), key, sizeof(key));
   assert(r == -1);
}

/* Cert present but key absent -> 0 (incomplete material, present nothing). */
static void test_cert_without_key_is_none(void)
{
   char crt[600], key[600], p[700];
   rm_material();
   snprintf(p, sizeof(p), "%s/tls/client.crt", g_home);
   write_file(p, 0600);
   int r = aimee_tls_client_cert_eligible(g_home, crt, sizeof(crt), key, sizeof(key));
   assert(r == 0);
}

/* Key is a symlink (even to a 0600 file) -> -1: never follow a symlinked key.
 * Defends against a symlink swapped in for the identity key. */
static void test_refuse_symlinked_key(void)
{
   char crt[600], key[600], p[700], tgt[700];
   rm_material();
   snprintf(p, sizeof(p), "%s/tls/client.crt", g_home);
   write_file(p, 0600);
   snprintf(tgt, sizeof(tgt), "%s/tls/real.key", g_home);
   write_file(tgt, 0600);
   snprintf(p, sizeof(p), "%s/tls/client.key", g_home);
   unlink(p);
   assert(symlink(tgt, p) == 0);
   int r = aimee_tls_client_cert_eligible(g_home, crt, sizeof(crt), key, sizeof(key));
   assert(r == -1);
   unlink(p);
   unlink(tgt);
}

/* Empty/NULL home -> 0 (a broken env must not crash or present anything). */
static void test_bad_home_is_none(void)
{
   char crt[600], key[600];
   assert(aimee_tls_client_cert_eligible(NULL, crt, sizeof(crt), key, sizeof(key)) == 0);
   assert(aimee_tls_client_cert_eligible("", crt, sizeof(crt), key, sizeof(key)) == 0);
}

int main(void)
{
   make_home();
   test_absent_is_none();
   test_present_when_0600();
   test_refuse_loose_key();
   test_refuse_symlinked_key();
   test_cert_without_key_is_none();
   test_bad_home_is_none();
   rm_material();
   printf("aimee_tls_clientcert: all tests passed\n");
   return 0;
}
