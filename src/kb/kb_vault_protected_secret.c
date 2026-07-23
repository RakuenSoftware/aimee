#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "kb_vault_protected_secret.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void kb_vault_protected_cleanse(void *data, size_t length)
{
   volatile uint8_t *p = data;
   while (p && length--)
      *p++ = 0;
}

void kb_vault_protected_secret_close(kb_vault_protected_secret_t *secret)
{
   if (!secret)
      return;
   if (secret->bytes && secret->mapped_length)
   {
      kb_vault_protected_cleanse(secret->bytes, secret->mapped_length);
      (void)munlock(secret->bytes, secret->mapped_length);
      (void)munmap(secret->bytes, secret->mapped_length);
   }
   memset(secret, 0, sizeof(*secret));
}

int kb_vault_protected_secret_open(kb_vault_protected_secret_t *secret, size_t capacity)
{
#if defined(__linux__) && defined(MADV_DONTDUMP) && defined(MADV_WIPEONFORK)
   if (!secret || !capacity)
      return -1;
   memset(secret, 0, sizeof(*secret));
   long page_value = sysconf(_SC_PAGESIZE);
   if (page_value <= 0 || (unsigned long)page_value > SIZE_MAX)
      return -1;
   size_t page = (size_t)page_value;
   if (capacity > SIZE_MAX - (page - 1))
      return -1;
   size_t mapped = (capacity + page - 1) / page * page;
   void *memory = mmap(NULL, mapped, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   if (memory == MAP_FAILED)
      return -1;
   if (mlock(memory, mapped) != 0 || madvise(memory, mapped, MADV_DONTDUMP) != 0 ||
       madvise(memory, mapped, MADV_WIPEONFORK) != 0)
   {
      int saved = errno;
      kb_vault_protected_cleanse(memory, mapped);
      (void)munlock(memory, mapped);
      (void)munmap(memory, mapped);
      errno = saved;
      return -1;
   }
   secret->bytes = memory;
   secret->capacity = capacity;
   secret->mapped_length = mapped;
   return 0;
#else
   (void)secret;
   (void)capacity;
   errno = ENOTSUP;
   return -1;
#endif
}

int kb_vault_protected_secret_set_length(kb_vault_protected_secret_t *secret, size_t length)
{
   if (!secret || !secret->bytes || !length || length > secret->capacity)
      return -1;
   secret->length = length;
   return 0;
}
