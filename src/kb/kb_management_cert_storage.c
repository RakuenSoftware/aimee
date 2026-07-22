#define _POSIX_C_SOURCE 200809L
#include "kb_management_cert_storage.h"
#include "kb_management_cert_codec.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int exact_hex(const char *s, size_t n)
{
   if (!s || strnlen(s, n + 1) != n)
      return 0;
   for (size_t i = 0; i < n; ++i)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
   return 1;
}

static int checked_dir(int fd)
{
   struct stat st;
   return fd >= 0 && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == 0 &&
          (st.st_mode & 0022) == 0;
}

static int checked_file(int fd)
{
   struct stat st;
   return fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 &&
          (st.st_mode & 0777) == 0600 && st.st_nlink == 1;
}

static kb_management_cert_storage_result_t open_error(int error, int missing_ok)
{
   if (missing_ok && error == ENOENT)
      return KB_MANAGEMENT_STORAGE_MISSING;
   if (error == ELOOP || error == ENOTDIR || error == EISDIR || error == ENXIO)
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
}

static int open_absolute_dir(const char *path)
{
   if (!path || path[0] != '/' || !path[1] || strnlen(path, PATH_MAX) >= PATH_MAX)
      return -1;
   int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
   if (fd < 0)
      return -1;
   const char *p = path + 1;
   while (*p)
   {
      const char *slash = strchr(p, '/');
      size_t n = slash ? (size_t)(slash - p) : strlen(p);
      if (!n || n > NAME_MAX || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.'))
      {
         close(fd);
         return -1;
      }
      char component[NAME_MAX + 1];
      memcpy(component, p, n);
      component[n] = 0;
      int next = openat(fd, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      close(fd);
      if (next < 0 || !checked_dir(next))
      {
         if (next >= 0)
            close(next);
         return -1;
      }
      fd = next;
      if (!slash)
         return fd;
      p = slash + 1;
   }
   close(fd);
   return -1;
}

kb_management_cert_storage_result_t
kb_management_cert_storage_open(const char *path, kb_management_cert_storage_t *out)
{
   if (!out)
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   out->dir_fd = -1;
   int fd = open_absolute_dir(path);
   if (fd < 0)
      return errno == EACCES || errno == EAGAIN ? KB_MANAGEMENT_STORAGE_UNAVAILABLE
                                                : KB_MANAGEMENT_STORAGE_INTEGRITY;
   if (flock(fd, LOCK_EX | LOCK_NB) != 0)
   {
      close(fd);
      return errno == EWOULDBLOCK ? KB_MANAGEMENT_STORAGE_CONFLICT
                                  : KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   }
   out->dir_fd = fd;
   return KB_MANAGEMENT_STORAGE_OK;
}

void kb_management_cert_storage_close(kb_management_cert_storage_t *storage)
{
   if (storage && storage->dir_fd >= 0)
   {
      flock(storage->dir_fd, LOCK_UN);
      close(storage->dir_fd);
      storage->dir_fd = -1;
   }
}

static int record_name(const char *kind, const char operation[65], char out[80])
{
   if ((!strcmp(kind, "intent") && exact_hex(operation, 64)) ||
       (!strcmp(kind, "candidate") && exact_hex(operation, 64)))
      return snprintf(out, 80, "%s.%s", kind, operation) == (int)(strlen(kind) + 1 + 64) ? 0 : -1;
   return -1;
}

static kb_management_cert_storage_result_t read_name(kb_management_cert_storage_t *storage,
                                                     const char *name, uint8_t *out, size_t cap,
                                                     size_t *out_len)
{
   if (out_len)
      *out_len = 0;
   if (out && cap)
      OPENSSL_cleanse(out, cap);
   if (!storage || storage->dir_fd < 0 || !name || !out || !cap || !out_len)
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   int fd = openat(storage->dir_fd, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
   if (fd < 0)
      return open_error(errno, 1);
   if (!checked_file(fd))
   {
      close(fd);
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   }
   size_t used = 0;
   while (used < cap)
   {
      ssize_t n = read(fd, out + used, cap - used);
      if (n > 0)
         used += (size_t)n;
      else if (!n)
         break;
      else if (errno != EINTR)
      {
         close(fd);
         OPENSSL_cleanse(out, cap);
         return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
      }
   }
   uint8_t extra;
   ssize_t more;
   do
      more = read(fd, &extra, 1);
   while (more < 0 && errno == EINTR);
   close(fd);
   if (!used || more != 0)
   {
      OPENSSL_cleanse(out, cap);
      return more < 0 ? KB_MANAGEMENT_STORAGE_UNAVAILABLE : KB_MANAGEMENT_STORAGE_INTEGRITY;
   }
   *out_len = used;
   return KB_MANAGEMENT_STORAGE_OK;
}

kb_management_cert_storage_result_t
kb_management_cert_storage_read(kb_management_cert_storage_t *storage, const char *kind,
                                const char operation[65], uint8_t *out, size_t cap, size_t *out_len)
{
   char name[80];
   if (record_name(kind, operation, name))
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   return read_name(storage, name, out, cap, out_len);
}

static int complete_write(int fd, const uint8_t *bytes, size_t len)
{
   size_t used = 0;
   while (used < len)
   {
      ssize_t n = write(fd, bytes + used, len - used);
      if (n > 0)
         used += (size_t)n;
      else if (n < 0 && errno == EINTR)
         continue;
      else
         return -1;
   }
   return 0;
}

static int verify_contents(int fd, const void *bytes, size_t len)
{
   if (len > KB_MANAGEMENT_CERT_CANDIDATE_MAX || lseek(fd, 0, SEEK_SET) < 0)
      return -1;
   uint8_t verify[KB_MANAGEMENT_CERT_CANDIDATE_MAX];
   size_t used = 0;
   while (used < len)
   {
      ssize_t n = read(fd, verify + used, len - used);
      if (n > 0)
         used += (size_t)n;
      else if (n < 0 && errno == EINTR)
         continue;
      else
         break;
   }
   uint8_t extra;
   ssize_t more;
   do
      more = read(fd, &extra, 1);
   while (more < 0 && errno == EINTR);
   int same = used == len && more == 0 && CRYPTO_memcmp(verify, bytes, len) == 0;
   OPENSSL_cleanse(verify, sizeof(verify));
   return same ? 0 : -1;
}

/* 1 exact, 0 well-formed byte mismatch, -1 I/O failure. */
static int compare_contents(int fd, const void *bytes, size_t len)
{
   if (len > 1024 || lseek(fd, 0, SEEK_SET) < 0)
      return -1;
   uint8_t existing[1024];
   size_t used = 0;
   while (used < len)
   {
      ssize_t n = read(fd, existing + used, len - used);
      if (n > 0)
         used += (size_t)n;
      else if (!n)
         break;
      else if (errno != EINTR)
      {
         OPENSSL_cleanse(existing, sizeof(existing));
         return -1;
      }
   }
   uint8_t extra;
   ssize_t more;
   do
      more = read(fd, &extra, 1);
   while (more < 0 && errno == EINTR);
   if (more < 0)
   {
      OPENSSL_cleanse(existing, sizeof(existing));
      return -1;
   }
   int exact = used == len && more == 0 && CRYPTO_memcmp(existing, bytes, len) == 0;
   OPENSSL_cleanse(existing, sizeof(existing));
   return exact;
}

static kb_management_cert_storage_result_t replay_existing(kb_management_cert_storage_t *storage,
                                                           const char *name, const void *bytes,
                                                           size_t len)
{
   int fd = openat(storage->dir_fd, name, O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
   if (fd < 0)
      return open_error(errno, 0);
   if (!checked_file(fd))
   {
      close(fd);
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   }
   uint8_t existing[KB_MANAGEMENT_CERT_CANDIDATE_MAX];
   size_t used = 0;
   while (used < sizeof(existing))
   {
      ssize_t n = read(fd, existing + used, sizeof(existing) - used);
      if (n > 0)
         used += (size_t)n;
      else if (!n)
         break;
      else if (errno != EINTR)
      {
         OPENSSL_cleanse(existing, sizeof(existing));
         close(fd);
         return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
      }
   }
   uint8_t extra;
   ssize_t more;
   do
      more = read(fd, &extra, 1);
   while (more < 0 && errno == EINTR);
   int exact = more == 0 && used == len && CRYPTO_memcmp(existing, bytes, len) == 0;
   OPENSSL_cleanse(existing, sizeof(existing));
   if (more < 0)
   {
      close(fd);
      return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   }
   if (!used || more > 0)
   {
      close(fd);
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   }
   if (!exact)
   {
      close(fd);
      return KB_MANAGEMENT_STORAGE_CONFLICT;
   }
   int sync_error = fdatasync(fd) != 0;
   if (close(fd) != 0)
      sync_error = 1;
   if (!sync_error && fsync(storage->dir_fd) != 0)
      sync_error = 1;
   return sync_error ? KB_MANAGEMENT_STORAGE_UNAVAILABLE : KB_MANAGEMENT_STORAGE_OK;
}

static kb_management_cert_storage_result_t
inspect_existing_no_replace(kb_management_cert_storage_t *storage, const char *name)
{
   int fd = openat(storage->dir_fd, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
   if (fd < 0)
      return open_error(errno, 0);
   int valid = checked_file(fd);
   close(fd);
   return valid ? KB_MANAGEMENT_STORAGE_CONFLICT : KB_MANAGEMENT_STORAGE_INTEGRITY;
}

kb_management_cert_storage_result_t
kb_management_cert_storage_stage(kb_management_cert_storage_t *storage, const char *kind,
                                 const char operation[65], const void *bytes, size_t len)
{
   char name[80];
   if (!storage || storage->dir_fd < 0 || record_name(kind, operation, name) || !bytes || !len ||
       len > KB_MANAGEMENT_CERT_CANDIDATE_MAX)
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   int fd = openat(storage->dir_fd, name, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
   if (fd < 0 && errno == EEXIST)
      return replay_existing(storage, name, bytes, len);
   if (fd < 0)
      return open_error(errno, 0);
   kb_management_cert_storage_result_t rc = KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   if (!checked_file(fd) || complete_write(fd, bytes, len) || fdatasync(fd))
      goto failed;
   if (verify_contents(fd, bytes, len))
   {
      close(fd);
      goto unlink_failed;
   }
   if (close(fd) || fsync(storage->dir_fd))
      goto unlink_failed;
   return KB_MANAGEMENT_STORAGE_OK;

failed:
   close(fd);
unlink_failed:
   if (unlinkat(storage->dir_fd, name, 0) == 0)
      fsync(storage->dir_fd);
   return rc;
}

static int random_hex(char out[33])
{
   static const char digits[] = "0123456789abcdef";
   uint8_t random[16];
   if (RAND_bytes(random, sizeof(random)) != 1)
      return -1;
   for (size_t i = 0; i < sizeof(random); ++i)
   {
      out[2 * i] = digits[random[i] >> 4];
      out[2 * i + 1] = digits[random[i] & 15];
   }
   out[32] = 0;
   OPENSSL_cleanse(random, sizeof(random));
   return 0;
}

kb_management_cert_storage_result_t
kb_management_cert_storage_promote(kb_management_cert_storage_t *storage, const void *bytes,
                                   size_t len)
{
   if (!storage || storage->dir_fd < 0 || !bytes || !len || len > 1024)
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   char suffix[33], name[48];
   if (random_hex(suffix) || snprintf(name, sizeof(name), "current.tmp.%s", suffix) != 44)
      return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   int fd = openat(storage->dir_fd, name, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
   if (fd < 0)
      return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   if (!checked_file(fd) || complete_write(fd, bytes, len) || fdatasync(fd) ||
       verify_contents(fd, bytes, len))
   {
      close(fd);
      unlinkat(storage->dir_fd, name, 0);
      fsync(storage->dir_fd);
      return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   }
   if (close(fd) || renameat(storage->dir_fd, name, storage->dir_fd, "current") ||
       fsync(storage->dir_fd))
   {
      unlinkat(storage->dir_fd, name, 0);
      fsync(storage->dir_fd);
      return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   }
   return KB_MANAGEMENT_STORAGE_OK;
}

kb_management_cert_storage_result_t
kb_management_cert_storage_current(kb_management_cert_storage_t *storage, uint8_t *out, size_t cap,
                                   size_t *out_len)
{
   return read_name(storage, "current", out, cap, out_len);
}

kb_management_cert_storage_result_t
kb_management_cert_storage_pending_publish(kb_management_cert_storage_t *storage, const void *bytes,
                                           size_t len)
{
   if (!storage || storage->dir_fd < 0 || !bytes || !len || len > 1024)
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   int fd =
       openat(storage->dir_fd, "pending", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
   if (fd < 0 && errno == EEXIST)
      return inspect_existing_no_replace(storage, "pending");
   if (fd < 0)
      return open_error(errno, 0);
   if (!checked_file(fd))
   {
      close(fd);
      if (unlinkat(storage->dir_fd, "pending", 0) == 0)
         fsync(storage->dir_fd);
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   }
   if (complete_write(fd, bytes, len) || fdatasync(fd) || verify_contents(fd, bytes, len))
   {
      close(fd);
      if (unlinkat(storage->dir_fd, "pending", 0) == 0)
         fsync(storage->dir_fd);
      return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   }
   if (!checked_file(fd))
   {
      close(fd);
      if (unlinkat(storage->dir_fd, "pending", 0) == 0)
         fsync(storage->dir_fd);
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   }
   int sync_error = close(fd) != 0;
   if (fsync(storage->dir_fd) != 0)
      sync_error = 1;
   return sync_error ? KB_MANAGEMENT_STORAGE_UNAVAILABLE : KB_MANAGEMENT_STORAGE_OK;
}

kb_management_cert_storage_result_t
kb_management_cert_storage_pending_read(kb_management_cert_storage_t *storage, uint8_t *out,
                                        size_t cap, size_t *out_len)
{
   return read_name(storage, "pending", out, cap, out_len);
}

kb_management_cert_storage_result_t
kb_management_cert_storage_pending_clear_exact(kb_management_cert_storage_t *storage,
                                               const void *bytes, size_t len)
{
   if (!storage || storage->dir_fd < 0 || !bytes || !len || len > 1024)
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   int fd = openat(storage->dir_fd, "pending", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
   if (fd < 0)
      return open_error(errno, 1);
   if (!checked_file(fd))
   {
      close(fd);
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   }
   struct stat opened, named;
   if (fstat(fd, &opened) != 0)
   {
      close(fd);
      return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   }
   int exact = compare_contents(fd, bytes, len);
   if (exact < 0)
   {
      close(fd);
      return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   }
   if (!exact)
   {
      close(fd);
      return KB_MANAGEMENT_STORAGE_CONFLICT;
   }
   int named_ok = fstatat(storage->dir_fd, "pending", &named, AT_SYMLINK_NOFOLLOW) == 0 &&
                  S_ISREG(named.st_mode) && named.st_uid == 0 && (named.st_mode & 0777) == 0600 &&
                  named.st_nlink == 1 && opened.st_dev == named.st_dev &&
                  opened.st_ino == named.st_ino;
   if (close(fd) != 0)
      return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   if (!named_ok)
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   if (unlinkat(storage->dir_fd, "pending", 0) != 0 || fsync(storage->dir_fd) != 0)
      return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   return KB_MANAGEMENT_STORAGE_OK;
}
