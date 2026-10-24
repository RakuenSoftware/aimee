#ifndef AIMEE_SCOPED_FILE_IO_H
#define AIMEE_SCOPED_FILE_IO_H
/* Lower resource I/O only: callers own policy, identity and final-action admission.
 * A held root and O_NOFOLLOW walk prevent a renamed/symlinked parent from
 * redirecting the operation. This code never selects a provider or grants access. */
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define SCOPED_FILE_IO_MAX (16u * 1024u * 1024u)
typedef struct
{
   char *bytes;
   size_t length;
   int effect_started;
} scoped_file_io_result_t;
static inline int scoped_file_parent_open(int root_fd, const char *root, const char *path,
                                          char **leaf, int create_parents, int (*live)(void *),
                                          void *context, scoped_file_io_result_t *result)
{
   size_t length = root ? strlen(root) : 0;
   if (root_fd < 0 || length < 2 || root[0] != '/' || !path || strncmp(path, root, length) ||
       path[length] != '/' || !path[length + 1])
      return -1;
   char *relative = strdup(path + length + 1);
   if (!relative)
      return -1;
   /* Validate the whole spelling before optional mkdir can create any parent. */
   for (const char *part = relative;;)
   {
      const char *slash = strchr(part, '/');
      size_t n = slash ? (size_t)(slash - part) : strlen(part);
      if (!n || (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.'))
      {
         free(relative);
         return -1;
      }
      if (!slash)
         break;
      part = slash + 1;
   }
   int fd = fcntl(root_fd, F_DUPFD_CLOEXEC, 3);
   char *component = relative;
   while (fd >= 0)
   {
      char *slash = strchr(component, '/');
      if (slash)
         *slash = 0;
      if (!component[0] || !strcmp(component, ".") || !strcmp(component, ".."))
         break;
      if (!slash)
      {
         *leaf = strdup(component);
         free(relative);
         if (*leaf)
            return fd;
         close(fd);
         return -1;
      }
      int next = openat(fd, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (next < 0 && errno == ENOENT && create_parents && (!live || live(context)))
      {
         if (!mkdirat(fd, component, 0700))
         {
            result->effect_started = 1;
            if (fsync(fd))
               break;
         }
         else if (errno != EEXIST)
            break;
         next = openat(fd, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      }
      close(fd);
      fd = next;
      component = slash + 1;
   }
   if (fd >= 0)
      close(fd);
   free(relative);
   return -1;
}
static inline int scoped_file_io_parents(int root_fd, const char *root, const char *path,
                                         int writing, const char *content, size_t length,
                                         int (*live)(void *), void *context,
                                         scoped_file_io_result_t *result, int create_parents)
{
   if (!result)
      return -1;
   memset(result, 0, sizeof *result);
   int parent = -1, fd = -1, rc = -1;
   char *leaf = NULL;
   if ((live && !live(context)) ||
       (writing && ((!content && length) || length > SCOPED_FILE_IO_MAX)) ||
       (parent = scoped_file_parent_open(root_fd, root, path, &leaf, writing && create_parents,
                                         live, context, result)) < 0)
      goto done;
   fd = openat(parent, leaf,
               (writing ? O_WRONLY | O_CREAT : O_RDONLY) | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK,
               0600);
   result->effect_started |= writing && fd >= 0;
   struct stat status;
   if (fd < 0 || fstat(fd, &status) || !S_ISREG(status.st_mode) || (live && !live(context)))
      goto done;
   if (writing)
   {
      if (ftruncate(fd, 0))
         goto done;
      size_t offset = 0;
      while (offset < length)
      {
         if (live && !live(context))
            goto done;
         ssize_t n = write(fd, content + offset, length - offset);
         if (n < 0 && errno == EINTR)
            continue;
         if (n <= 0)
            goto done;
         offset += (size_t)n;
      }
      if (fsync(fd))
         goto done;
   }
   else
   {
      if (status.st_size < 0 || (unsigned long long)status.st_size > SCOPED_FILE_IO_MAX ||
          !(result->bytes = malloc(SCOPED_FILE_IO_MAX + 1)))
         goto done;
      for (;;)
      {
         if (live && !live(context))
            goto done;
         ssize_t n =
             read(fd, result->bytes + result->length, SCOPED_FILE_IO_MAX + 1 - result->length);
         if (n < 0 && errno == EINTR)
            continue;
         if (n < 0 || result->length + (size_t)n > SCOPED_FILE_IO_MAX)
            goto done;
         if (!n)
            break;
         result->length += (size_t)n;
      }
      result->bytes[result->length] = 0;
   }
   if (!live || live(context))
      rc = 0;
done:
   free(leaf);
   if (fd >= 0 && close(fd))
      rc = -1;
   if (parent >= 0)
      close(parent);
   return rc;
}
/* Existing callers retain the no-parent-creation contract. */
static inline int scoped_file_io(int root_fd, const char *root, const char *path, int writing,
                                 const char *content, size_t length, int (*live)(void *),
                                 void *context, scoped_file_io_result_t *result)
{
   return scoped_file_io_parents(root_fd, root, path, writing, content, length, live, context,
                                 result, 0);
}

/* NUL-delimited Unix names preserve embedded newlines without a shell/parser.
 * A partial directory never becomes a successful complete listing. */
static inline int scoped_directory_io(int root_fd, const char *root, const char *path,
                                      scoped_file_io_result_t *result)
{
   if (!result)
      return -1;
   memset(result, 0, sizeof *result);
   if (!root || !path)
      return -1;
   char *normalized = strdup(path), *leaf = NULL;
   if (!normalized)
      return -1;
   size_t length = strlen(normalized), root_length = strlen(root);
   while (length > root_length)
   {
      if (normalized[length - 1] == '/')
         normalized[--length] = 0;
      else if (length >= 2 && !strcmp(normalized + length - 2, "/."))
         normalized[length -= 2] = 0;
      else
         break;
   }
   int parent = -1, fd = -1;
   if (!strcmp(normalized, root))
      fd = openat(root_fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   else if ((parent = scoped_file_parent_open(root_fd, root, normalized, &leaf, 0, NULL, NULL,
                                              result)) >= 0)
      fd = openat(parent, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   free(normalized);
   free(leaf);
   if (parent >= 0)
      close(parent);
   if (fd < 0)
      return -1;
   DIR *directory = fdopendir(fd);
   if (!directory)
   {
      close(fd);
      return -1;
   }
   int rc = -1;
   size_t capacity = 4096, count = 0;
   result->bytes = malloc(capacity);
   if (!result->bytes)
      goto done;
   for (;;)
   {
      errno = 0;
      struct dirent *entry = readdir(directory);
      if (!entry)
      {
         if (!errno)
            rc = 0;
         break;
      }
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
         continue;
      size_t n = strlen(entry->d_name) + 1;
      if (++count > 65536 || n > SCOPED_FILE_IO_MAX - result->length)
         break;
      if (result->length + n > capacity)
      {
         size_t next = capacity * 2;
         if (next > SCOPED_FILE_IO_MAX)
            next = SCOPED_FILE_IO_MAX;
         char *bytes = realloc(result->bytes, next);
         if (!bytes)
            break;
         result->bytes = bytes;
         capacity = next;
      }
      memcpy(result->bytes + result->length, entry->d_name, n);
      result->length += n;
   }
done:
   if (closedir(directory))
      rc = -1;
   return rc;
}
#endif
