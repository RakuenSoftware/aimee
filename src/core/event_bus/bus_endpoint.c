#define _GNU_SOURCE
#include <aimee/core/event_bus/bus_endpoint.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int endpoint_address(const char *path, struct sockaddr_un *address, socklen_t *length)
{
   size_t path_length = path ? strnlen(path, sizeof(address->sun_path)) : 0;
   if (!path_length || path_length == sizeof(address->sun_path) || path[0] != '/')
   {
      errno = EINVAL;
      return -1;
   }
   memset(address, 0, sizeof(*address));
   address->sun_family = AF_UNIX;
   memcpy(address->sun_path, path, path_length + 1U);
   *length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_length + 1U);
   return 0;
}

static int endpoint_socket(void)
{
   int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
   if (fd >= 0)
      return fd;
   if (errno != EINVAL && errno != EPROTONOSUPPORT)
      return -1;
   fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
   if (fd >= 0)
   {
      int flags = fcntl(fd, F_GETFD);
      if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0)
      {
         close(fd);
         return -1;
      }
   }
   return fd;
}

int bus_endpoint_remove(const char *path)
{
   struct stat status;
   if (!path || path[0] != '/')
   {
      errno = EINVAL;
      return -1;
   }
   if (lstat(path, &status) != 0)
      return errno == ENOENT ? 0 : -1;
   if (!S_ISSOCK(status.st_mode) || status.st_uid != geteuid())
   {
      errno = EADDRINUSE;
      return -1;
   }
   return unlink(path);
}

int bus_endpoint_listen(const char *path, unsigned mode, int backlog, int *out_fd)
{
   if (out_fd)
      *out_fd = -1;
   struct sockaddr_un address;
   socklen_t length;
   if (!out_fd || mode > 0777U || backlog <= 0 || endpoint_address(path, &address, &length) != 0)
      return -1;
   int fd = endpoint_socket();
   if (fd < 0)
      return -1;
   mode_t old_mask = umask(0777U);
   int result = bind(fd, (const struct sockaddr *)&address, length);
   umask(old_mask);
   int bound = result == 0;
   if (result == 0)
      result = chmod(path, (mode_t)mode);
   if (result == 0)
      result = listen(fd, backlog);
   if (result != 0)
   {
      int saved = errno;
      close(fd);
      /* Never unlink somebody else's live listener after a failed bind. */
      if (bound)
         (void)bus_endpoint_remove(path);
      errno = saved;
      return -1;
   }
   *out_fd = fd;
   return 0;
}

int bus_endpoint_accept(int listener_fd, int *out_fd)
{
   if (out_fd)
      *out_fd = -1;
   if (listener_fd < 0 || !out_fd)
   {
      errno = EINVAL;
      return -1;
   }
   int fd;
   do
      fd = accept4(listener_fd, NULL, NULL, SOCK_CLOEXEC);
   while (fd < 0 && errno == EINTR);
   if (fd < 0 && (errno == ENOSYS || errno == EINVAL))
   {
      do
         fd = accept(listener_fd, NULL, NULL);
      while (fd < 0 && errno == EINTR);
      if (fd >= 0)
      {
         int flags = fcntl(fd, F_GETFD);
         if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0)
         {
            close(fd);
            return -1;
         }
      }
   }
   if (fd < 0)
      return -1;
   *out_fd = fd;
   return 0;
}

int bus_endpoint_connect(const char *path, int *out_fd)
{
   if (out_fd)
      *out_fd = -1;
   struct sockaddr_un address;
   socklen_t length;
   if (!out_fd || endpoint_address(path, &address, &length) != 0)
      return -1;
   int fd = endpoint_socket();
   if (fd < 0)
      return -1;
   if (connect(fd, (const struct sockaddr *)&address, length) != 0)
   {
      int saved = errno;
      close(fd);
      errno = saved;
      return -1;
   }
   *out_fd = fd;
   return 0;
}

int bus_endpoint_close(int *fd)
{
   if (!fd || *fd < 0)
      return 0;
   int result = close(*fd);
   *fd = -1;
   return result;
}
