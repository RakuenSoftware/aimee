/* test_vault_server_key.c: WP-C.4 — the server-sealed KEK that lets aimee-server
 * decrypt dual-access credentials autonomously. Runs against a throwaway
 * AIMEE_HOME. Pins: derive succeeds + is non-trivial; the KEK is stable across
 * calls (cached) AND across a cache reset (re-derived from the persisted master
 * key file, i.e. survives a "restart"); the master key file is 0600 and exactly
 * 32 bytes. */
#include "vault_server_key.h"
#include "vault_crypto.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_home[256];

static int is_all_zero(const uint8_t *p, size_t n)
{
   for (size_t i = 0; i < n; i++)
      if (p[i])
         return 0;
   return 1;
}

static void test_derive_nontrivial(void)
{
   uint8_t kek[VAULT_KEK_LEN];
   assert(vault_server_kek(kek) == 0);
   assert(!is_all_zero(kek, sizeof(kek))); /* a real derived key, not a zero buffer */
   printf("  PASS: test_derive_nontrivial\n");
}

static void test_stable_cached(void)
{
   uint8_t a[VAULT_KEK_LEN], b[VAULT_KEK_LEN];
   assert(vault_server_kek(a) == 0);
   assert(vault_server_kek(b) == 0);
   assert(memcmp(a, b, sizeof(a)) == 0); /* same KEK within a process */
   printf("  PASS: test_stable_cached\n");
}

static void test_survives_restart(void)
{
   uint8_t before[VAULT_KEK_LEN], after[VAULT_KEK_LEN];
   assert(vault_server_kek(before) == 0);
   /* Simulate a process restart: drop the in-RAM cache, re-derive from the
    * persisted master key file. The KEK must be identical (else every restart
    * would orphan all server wraps). */
   vault_server_key_reset_for_test();
   assert(vault_server_kek(after) == 0);
   assert(memcmp(before, after, sizeof(before)) == 0);
   printf("  PASS: test_survives_restart\n");
}

static void test_master_key_file_locked_down(void)
{
   uint8_t kek[VAULT_KEK_LEN];
   assert(vault_server_kek(kek) == 0); /* ensure the file exists */
   char path[512];
   snprintf(path, sizeof(path), "%s/.vault/.server-master.key", g_home);
   struct stat st;
   assert(stat(path, &st) == 0);
   assert(st.st_size == VAULT_ROOT_KEY_LEN); /* exactly 32 bytes */
   assert((st.st_mode & 0777) == 0600);      /* owner-only */
   printf("  PASS: test_master_key_file_locked_down\n");
}

int main(void)
{
   snprintf(g_home, sizeof(g_home), "/tmp/aimee-svrkey-test-%d", (int)getpid());
   char mk[320];
   snprintf(mk, sizeof(mk), "rm -rf %s && mkdir -p %s", g_home, g_home);
   assert(system(mk) == 0);
   setenv("AIMEE_HOME", g_home, 1);

   test_derive_nontrivial();
   test_stable_cached();
   test_survives_restart();
   test_master_key_file_locked_down();

   char rm[320];
   snprintf(rm, sizeof(rm), "rm -rf %s", g_home);
   (void)system(rm);
   printf("vault_server_key: all tests passed\n");
   return 0;
}
