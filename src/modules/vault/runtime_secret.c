/* runtime_secret.c — process-memory cache for Vault-sourced credentials. */
#include "runtime_secret.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/mman.h>
#endif

#define RUNTIME_SECRET_MAX       1024
#define RUNTIME_SECRET_NAME_MAX  128
#define RUNTIME_SECRET_VALUE_MAX 4096

typedef struct
{
   char name[RUNTIME_SECRET_NAME_MAX];
   char *value;
   size_t len;
} runtime_secret_entry_t;

static runtime_secret_entry_t g_entries[RUNTIME_SECRET_MAX];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void runtime_secret_wipe(void *ptr, size_t len)
{
   volatile unsigned char *p = ptr;
   while (len--)
      *p++ = 0;
}

static void secret_free(runtime_secret_entry_t *entry)
{
   if (!entry || !entry->value)
      return;
   runtime_secret_wipe(entry->value, entry->len);
#ifndef _WIN32
   (void)munlock(entry->value, entry->len + 1);
#endif
   free(entry->value);
   memset(entry, 0, sizeof(*entry));
}

int runtime_secret_store(const char *name, const char *value)
{
   if (!name || !name[0] || !value || !value[0] || strlen(name) >= RUNTIME_SECRET_NAME_MAX)
      return -1;
   size_t len = strlen(value);
   if (len >= RUNTIME_SECRET_VALUE_MAX)
      return -1;
   char *copy = malloc(len + 1);
   if (!copy)
      return -1;
   memcpy(copy, value, len + 1);
#ifndef _WIN32
   (void)mlock(copy, len + 1);
#ifdef MADV_DONTDUMP
   (void)madvise(copy, len + 1, MADV_DONTDUMP);
#endif
#endif

   pthread_mutex_lock(&g_lock);
   runtime_secret_entry_t *slot = NULL;
   for (int i = 0; i < RUNTIME_SECRET_MAX; i++)
   {
      if (g_entries[i].value && strcmp(g_entries[i].name, name) == 0)
      {
         slot = &g_entries[i];
         break;
      }
      if (!slot && !g_entries[i].value)
         slot = &g_entries[i];
   }
   if (!slot)
   {
      pthread_mutex_unlock(&g_lock);
      runtime_secret_wipe(copy, len);
#ifndef _WIN32
      (void)munlock(copy, len + 1);
#endif
      free(copy);
      return -1;
   }
   secret_free(slot);
   snprintf(slot->name, sizeof(slot->name), "%s", name);
   slot->value = copy;
   slot->len = len;
   pthread_mutex_unlock(&g_lock);
   return 0;
}

int runtime_secret_get(const char *name, char *out, size_t out_len)
{
   if (out && out_len)
      out[0] = '\0';
   if (!name || !out || out_len == 0)
      return 0;
   int found = 0;
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < RUNTIME_SECRET_MAX; i++)
      if (g_entries[i].value && strcmp(g_entries[i].name, name) == 0)
      {
         if (g_entries[i].len < out_len)
         {
            memcpy(out, g_entries[i].value, g_entries[i].len + 1);
            found = 1;
         }
         break;
      }
   pthread_mutex_unlock(&g_lock);
   return found;
}

int runtime_secret_has(const char *name)
{
   if (!name)
      return 0;
   int found = 0;
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < RUNTIME_SECRET_MAX; i++)
      if (g_entries[i].value && strcmp(g_entries[i].name, name) == 0)
      {
         found = 1;
         break;
      }
   pthread_mutex_unlock(&g_lock);
   return found;
}

void runtime_secret_remove(const char *name)
{
   if (!name)
      return;
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < RUNTIME_SECRET_MAX; i++)
      if (g_entries[i].value && strcmp(g_entries[i].name, name) == 0)
      {
         secret_free(&g_entries[i]);
         break;
      }
   pthread_mutex_unlock(&g_lock);
}

void runtime_secret_clear(void)
{
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < RUNTIME_SECRET_MAX; i++)
      secret_free(&g_entries[i]);
   pthread_mutex_unlock(&g_lock);
}
