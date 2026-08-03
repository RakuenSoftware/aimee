#define _GNU_SOURCE
#include <aimee/core/event_bus/bus_attach.h>

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int bus_fd_send(int sock, const void *payload, size_t len, const int *fds, int n)
{
   if (n < 0 || n > 3)
   {
      errno = EINVAL;
      return -1;
   }
   struct iovec iov = {.iov_base = (void *)payload, .iov_len = len};
   union
   {
      struct cmsghdr align;
      char buf[CMSG_SPACE(sizeof(int) * 3)];
   } cbuf;
   memset(&cbuf, 0, sizeof cbuf);
   struct msghdr msg;
   memset(&msg, 0, sizeof msg);
   msg.msg_iov = &iov;
   msg.msg_iovlen = 1;
   if (n > 0)
   {
      msg.msg_control = cbuf.buf;
      msg.msg_controllen = CMSG_SPACE(sizeof(int) * n);
      struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
      c->cmsg_level = SOL_SOCKET;
      c->cmsg_type = SCM_RIGHTS;
      c->cmsg_len = CMSG_LEN(sizeof(int) * n);
      memcpy(CMSG_DATA(c), fds, sizeof(int) * n);
   }
   ssize_t result;
   do
      result = sendmsg(sock, &msg, 0);
   while (result < 0 && errno == EINTR);
   return result < 0 ? -1 : 0;
}

long bus_fd_recv(int sock, void *payload, size_t len, int *fds, int max_fds, int *n_out)
{
   if (max_fds < 0 || max_fds > 3)
   {
      errno = EINVAL;
      return -1;
   }
   if (n_out)
      *n_out = 0;
   struct iovec iov = {.iov_base = payload, .iov_len = len};
   union
   {
      struct cmsghdr align;
      char buf[CMSG_SPACE(sizeof(int) * 3)];
   } cbuf;
   memset(&cbuf, 0, sizeof cbuf);
   struct msghdr msg;
   memset(&msg, 0, sizeof msg);
   msg.msg_iov = &iov;
   msg.msg_iovlen = 1;
   msg.msg_control = cbuf.buf;
   msg.msg_controllen = sizeof cbuf.buf;
   ssize_t result;
   do
      result = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
   while (result < 0 && errno == EINTR);
   if (result < 0)
      return -1;
   int got = 0;
   for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
      if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
      {
         int count = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
         const int *incoming = (const int *)CMSG_DATA(c);
         for (int i = 0; i < count; i++)
         {
            if (got < max_fds && !(msg.msg_flags & MSG_CTRUNC))
               fds[got++] = incoming[i];
            else
               close(incoming[i]);
         }
      }
   if (msg.msg_flags & MSG_CTRUNC)
   {
      for (int i = 0; i < got; i++)
         close(fds[i]);
      errno = EPROTO;
      return -1;
   }
   if (n_out)
      *n_out = got;
   return result;
}
