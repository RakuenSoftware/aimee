#include "kb/kb_vault_protected_secret.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void test_arena_and_cleanse(void)
{
   kb_vault_protected_secret_t secret;
   assert(kb_vault_protected_secret_open(&secret, 4096) == 0);
   assert(secret.bytes && secret.capacity == 4096 && secret.mapped_length >= secret.capacity);
   memset(secret.bytes, 0xa5, secret.capacity);
   assert(kb_vault_protected_secret_set_length(&secret, 32) == 0 && secret.length == 32);
   assert(kb_vault_protected_secret_set_length(&secret, 0) == -1);
   assert(kb_vault_protected_secret_set_length(&secret, 4097) == -1);
   kb_vault_protected_cleanse(secret.bytes, secret.capacity);
   for (size_t i = 0; i < secret.capacity; ++i)
      assert(secret.bytes[i] == 0);
   kb_vault_protected_secret_close(&secret);
   assert(!secret.bytes && !secret.length && !secret.capacity && !secret.mapped_length);
}

static void test_wipe_on_fork(void)
{
   kb_vault_protected_secret_t secret;
   assert(kb_vault_protected_secret_open(&secret, 64) == 0);
   memset(secret.bytes, 0x5a, 64);
   assert(kb_vault_protected_secret_set_length(&secret, 64) == 0);
   pid_t child = fork();
   assert(child >= 0);
   if (child == 0)
   {
      uint8_t any = 0;
      for (size_t i = 0; i < secret.length; ++i)
         any |= secret.bytes[i];
      _exit(any ? 1 : 0);
   }
   int status = 0;
   assert(waitpid(child, &status, 0) == child);
   assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
   for (size_t i = 0; i < secret.length; ++i)
      assert(secret.bytes[i] == 0x5a);
   kb_vault_protected_secret_close(&secret);
}

int main(void)
{
   test_arena_and_cleanse();
   test_wipe_on_fork();
   puts("kb_vault_protected_secret: all tests passed");
   return 0;
}
