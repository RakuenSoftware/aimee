/* Session-scoped hook identity. See hook_session_token.h. */
#include "hook_session_token.h"
#include "platform_random.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

#define HOOK_TOKEN_SLOTS 512
#define HOOK_TOKEN_TTL   (24 * 60 * 60)

typedef struct
{
   int used;
   char session_id[64];
   char client[32];
   char principal[128];
   char token[HOOK_SESSION_TOKEN_CAP];
   time_t expires_at;
} hook_token_slot_t;

static hook_token_slot_t g_tokens[HOOK_TOKEN_SLOTS];
#ifdef _WIN32
static SRWLOCK g_tokens_mu = SRWLOCK_INIT;
#define TOKEN_LOCK()   AcquireSRWLockExclusive(&g_tokens_mu)
#define TOKEN_UNLOCK() ReleaseSRWLockExclusive(&g_tokens_mu)
#else
static pthread_mutex_t g_tokens_mu = PTHREAD_MUTEX_INITIALIZER;
#define TOKEN_LOCK()   pthread_mutex_lock(&g_tokens_mu)
#define TOKEN_UNLOCK() pthread_mutex_unlock(&g_tokens_mu)
#endif

static int safe_component(const char *value, size_t max_len)
{
   if (!value || !value[0] || strlen(value) > max_len)
      return 0;
   for (const unsigned char *p = (const unsigned char *)value; *p; p++)
      if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.'))
         return 0;
   return 1;
}

static int token_shape_valid(const char *token)
{
   if (!token || strlen(token) != HOOK_SESSION_TOKEN_HEX_LEN)
      return 0;
   for (const unsigned char *p = (const unsigned char *)token; *p; p++)
      if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
         return 0;
   return 1;
}

static int token_equal(const char *a, const char *b)
{
   unsigned char different = 0;
   for (size_t i = 0; i < HOOK_SESSION_TOKEN_HEX_LEN; i++)
      different |= (unsigned char)a[i] ^ (unsigned char)b[i];
   return different == 0;
}

static int binding_valid(const char *session_id, const char *client, const char *principal)
{
   return safe_component(session_id, 63) && safe_component(client, 31) && principal &&
          principal[0] && strlen(principal) < 128;
}

int hook_session_token_mint(const char *session_id, const char *client, const char *principal,
                            char out[HOOK_SESSION_TOKEN_CAP], time_t *expires_at)
{
   if (out)
      out[0] = '\0';
   if (!out || !binding_valid(session_id, client, principal) ||
       platform_random_hex(out, HOOK_SESSION_TOKEN_HEX_LEN) != 0)
      return -1;

   time_t now = time(NULL);
   time_t expiry = now + HOOK_TOKEN_TTL;
   TOKEN_LOCK();
   int chosen = -1;
   int oldest = 0;
   for (int i = 0; i < HOOK_TOKEN_SLOTS; i++)
   {
      if (g_tokens[i].used && g_tokens[i].expires_at <= now)
      {
         memset(&g_tokens[i], 0, sizeof(g_tokens[i]));
         if (chosen < 0)
            chosen = i;
      }
      if (g_tokens[i].used && !strcmp(g_tokens[i].session_id, session_id) &&
          !strcmp(g_tokens[i].client, client) && !strcmp(g_tokens[i].principal, principal))
      {
         chosen = i;
         break;
      }
      if (!g_tokens[i].used && chosen < 0)
         chosen = i;
      if (g_tokens[i].expires_at < g_tokens[oldest].expires_at)
         oldest = i;
   }
   if (chosen < 0)
      chosen = oldest;
   hook_token_slot_t *slot = &g_tokens[chosen];
   memset(slot, 0, sizeof(*slot));
   slot->used = 1;
   snprintf(slot->session_id, sizeof(slot->session_id), "%s", session_id);
   snprintf(slot->client, sizeof(slot->client), "%s", client);
   snprintf(slot->principal, sizeof(slot->principal), "%s", principal);
   snprintf(slot->token, sizeof(slot->token), "%s", out);
   slot->expires_at = expiry;
   TOKEN_UNLOCK();
   if (expires_at)
      *expires_at = expiry;
   return 0;
}

int hook_session_token_verify(const char *session_id, const char *client, const char *principal,
                              const char *token)
{
   if (!binding_valid(session_id, client, principal) || !token_shape_valid(token))
      return 0;
   int valid = 0;
   time_t now = time(NULL);
   TOKEN_LOCK();
   for (int i = 0; i < HOOK_TOKEN_SLOTS; i++)
   {
      hook_token_slot_t *slot = &g_tokens[i];
      if (slot->used && slot->expires_at <= now)
      {
         memset(slot, 0, sizeof(*slot));
         continue;
      }
      if (slot->used && !strcmp(slot->session_id, session_id) && !strcmp(slot->client, client) &&
          !strcmp(slot->principal, principal))
      {
         valid = token_equal(slot->token, token);
         break;
      }
   }
   TOKEN_UNLOCK();
   return valid;
}

void hook_session_token_revoke(const char *session_id, const char *client, const char *principal)
{
   if (!binding_valid(session_id, client, principal))
      return;
   TOKEN_LOCK();
   for (int i = 0; i < HOOK_TOKEN_SLOTS; i++)
      if (g_tokens[i].used && !strcmp(g_tokens[i].session_id, session_id) &&
          !strcmp(g_tokens[i].client, client) && !strcmp(g_tokens[i].principal, principal))
         memset(&g_tokens[i], 0, sizeof(g_tokens[i]));
   TOKEN_UNLOCK();
}

void hook_session_token_registry_reset(void)
{
   TOKEN_LOCK();
   memset(g_tokens, 0, sizeof(g_tokens));
   TOKEN_UNLOCK();
}

static int token_path(char *out, size_t cap, const char *home, const char *session_id,
                      const char *client, int directory)
{
   if (!out || !home || !home[0] || !safe_component(session_id, 63) || !safe_component(client, 31))
      return -1;
   int n = directory ? snprintf(out, cap, "%s/hook-tokens", home)
                     : snprintf(out, cap, "%s/hook-tokens/%s.%s", home, session_id, client);
   return n > 0 && (size_t)n < cap ? 0 : -1;
}

int hook_session_token_store(const char *home, const char *session_id, const char *client,
                             const char *token)
{
   if (!token_shape_valid(token))
      return -1;
   char dir[4096], path[4096], tmp[4096];
   if (token_path(dir, sizeof(dir), home, session_id, client, 1) != 0 ||
       token_path(path, sizeof(path), home, session_id, client, 0) != 0)
      return -1;
#ifdef _WIN32
   if (snprintf(tmp, sizeof(tmp), "%s.tmp.%lu", path, (unsigned long)GetCurrentProcessId()) >=
       (int)sizeof(tmp))
      return -1;
   if (!CreateDirectoryA(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
      return -1;
   HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                          FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
   if (h == INVALID_HANDLE_VALUE)
      return -1;
   DWORD wrote = 0;
   size_t len = strlen(token);
   int ok = WriteFile(h, token, (DWORD)len, &wrote, NULL) && wrote == (DWORD)len &&
            WriteFile(h, "\n", 1, &wrote, NULL) && wrote == 1 && FlushFileBuffers(h);
   if (!CloseHandle(h))
      ok = 0;
   if (ok && !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      ok = 0;
   if (!ok)
   {
      DeleteFileA(tmp);
      return -1;
   }
   return 0;
#else
   if (snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof(tmp))
      return -1;
   if (mkdir(dir, 0700) != 0 && errno != EEXIST)
      return -1;
   if (chmod(dir, 0700) != 0)
      return -1;
   int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
   if (fd < 0)
      return -1;
   size_t len = strlen(token);
   int ok = write(fd, token, len) == (ssize_t)len && write(fd, "\n", 1) == 1 && fsync(fd) == 0 &&
            close(fd) == 0 && rename(tmp, path) == 0;
   if (!ok)
   {
      close(fd);
      unlink(tmp);
      return -1;
   }
   return chmod(path, 0600) == 0 ? 0 : -1;
#endif
}

int hook_session_token_load(const char *home, const char *session_id, const char *client,
                            char out[HOOK_SESSION_TOKEN_CAP])
{
   if (out)
      out[0] = '\0';
   char path[4096];
   if (!out || token_path(path, sizeof(path), home, session_id, client, 0) != 0)
      return -1;
#ifdef _WIN32
   HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                          FILE_FLAG_OPEN_REPARSE_POINT, NULL);
   if (h == INVALID_HANDLE_VALUE)
      return -1;
   BY_HANDLE_FILE_INFORMATION info;
   char buf[HOOK_SESSION_TOKEN_CAP + 2];
   DWORD n = 0;
   int safe = GetFileInformationByHandle(h, &info) &&
              !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && info.nFileSizeHigh == 0 &&
              info.nFileSizeLow <= HOOK_SESSION_TOKEN_HEX_LEN + 1;
   int read_ok = safe && ReadFile(h, buf, sizeof(buf) - 1, &n, NULL);
   CloseHandle(h);
   if (!read_ok || n > HOOK_SESSION_TOKEN_HEX_LEN + 1)
      return -1;
#else
   int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
   if (fd < 0)
      return -1;
   struct stat st;
   char buf[HOOK_SESSION_TOKEN_CAP + 2];
   ssize_t n = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && !(st.st_mode & 0077)
                   ? read(fd, buf, sizeof(buf) - 1)
                   : -1;
   close(fd);
   if (n < 0 || n > HOOK_SESSION_TOKEN_HEX_LEN + 1)
      return -1;
#endif
   buf[n] = '\0';
   buf[strcspn(buf, "\r\n")] = '\0';
   if (!token_shape_valid(buf))
      return -1;
   snprintf(out, HOOK_SESSION_TOKEN_CAP, "%s", buf);
   memset(buf, 0, sizeof(buf));
   return 0;
}

int hook_session_token_delete(const char *home, const char *session_id, const char *client)
{
   char path[4096];
   if (token_path(path, sizeof(path), home, session_id, client, 0) != 0)
      return -1;
#ifdef _WIN32
   return DeleteFileA(path) || GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
#else
   return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
#endif
}
