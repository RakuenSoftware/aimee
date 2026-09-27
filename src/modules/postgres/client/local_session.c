/* Private bootstrap/operator transport. All database I/O stays in postgres. */
#define _GNU_SOURCE
#include "session_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef AIMEE_POSTGRES_LOCAL_PROVIDER
#define AIMEE_POSTGRES_LOCAL_PROVIDER "/usr/local/libexec/aimee-modules/aimee-module-postgres"
#endif
#define PROVIDER AIMEE_POSTGRES_LOCAL_PROVIDER
static int64_t milliseconds(void)
{
   struct timespec now;
   if (clock_gettime(CLOCK_MONOTONIC, &now))
      return -1;
   return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static int transfer(int fd, void *buffer, size_t length, int writing, int64_t deadline)
{
   unsigned char *p = buffer;
   while (length)
   {
      int64_t now = milliseconds();
      if (now < 0 || now >= deadline)
         return -1;
      struct pollfd pollfd = {.fd = fd, .events = writing ? POLLOUT : POLLIN};
      int rc = poll(&pollfd, 1, (int)(deadline - now));
      if (rc < 0 && errno == EINTR)
         continue;
      if (rc <= 0 || !(pollfd.revents & pollfd.events))
         return -1;
      ssize_t n = writing ? send(fd, p, length, MSG_NOSIGNAL | MSG_DONTWAIT)
                          : recv(fd, p, length, MSG_DONTWAIT);
      if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
         continue;
      if (n <= 0)
         return -1;
      p += n;
      length -= (size_t)n;
   }
   return 0;
}
static int write_frame(int fd, const void *body, uint32_t length, int64_t deadline)
{
   unsigned char bytes[4];
   for (unsigned i = 0; i < 4; i++)
      bytes[i] = (unsigned char)(length >> (8 * i));
   return transfer(fd, bytes, 4, 1, deadline) || transfer(fd, (void *)body, length, 1, deadline)
              ? -1
              : 0;
}
void aimee_postgres_local_stop(aimee_postgres_session_t *s)
{
   if (s->channel >= 0)
   {
      close(s->channel);
      s->channel = -2;
   }
   if (s->child <= 0)
      return;
   int status;
   if (s->deadline_ns && milliseconds() >= (int64_t)(s->deadline_ns / 1000000))
      kill(s->child, SIGKILL);
   for (unsigned i = 0; i < 100; i++)
   {
      pid_t done = waitpid(s->child, &status, WNOHANG);
      if (done == s->child || (done < 0 && errno == ECHILD))
      {
         s->child = 0;
         return;
      }
      struct timespec tick = {.tv_nsec = 10000000};
      nanosleep(&tick, NULL);
   }
   /* An unresponsive provider must not outlive its private authority channel. */
   kill(s->child, SIGKILL);
   while (waitpid(s->child, &status, 0) < 0 && errno == EINTR)
   {
   }
   s->child = 0;
}
int aimee_postgres_local_start(aimee_postgres_session_t *s, const char *credential)
{
   if (!credential || (!*credential && s->local_policy != 2 && s->local_policy != 3) ||
       strlen(credential) > 4095)
      return -1;
   int pair[2];
   int socket_flags = SOCK_STREAM;
#ifdef SOCK_CLOEXEC
   socket_flags |= SOCK_CLOEXEC;
#endif
   if (socketpair(AF_UNIX, socket_flags, 0, pair))
      return -1;
   for (unsigned i = 0; i < 2; i++)
   {
      if (fcntl(pair[i], F_SETFD, FD_CLOEXEC) < 0)
      {
         close(pair[0]);
         close(pair[1]);
         return -1;
      }
#ifdef SO_NOSIGPIPE
      int enabled = 1;
      if (setsockopt(pair[i], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)))
      {
         close(pair[0]);
         close(pair[1]);
         return -1;
      }
#endif
   }
   for (unsigned i = 0; i < 2; i++)
      if (pair[i] < 3)
      {
         int next = fcntl(pair[i], F_DUPFD_CLOEXEC, 3);
         close(pair[i]);
         pair[i] = next;
         if (next < 0)
         {
            close(pair[1 - i]);
            return -1;
         }
      }
   posix_spawn_file_actions_t actions;
   int rc = posix_spawn_file_actions_init(&actions);
   if (rc)
   {
      close(pair[0]);
      close(pair[1]);
      return -1;
   }
   rc = posix_spawn_file_actions_adddup2(&actions, pair[1], STDIN_FILENO);
   if (!rc)
      rc = posix_spawn_file_actions_adddup2(&actions, pair[1], STDOUT_FILENO);
   if (!rc)
      rc = posix_spawn_file_actions_addclose(&actions, pair[0]);
   if (!rc)
      rc = posix_spawn_file_actions_addclose(&actions, pair[1]);
   char *argv[] = {PROVIDER, "__aimee_postgres_local_session", NULL};
   if (!rc)
      rc = posix_spawn(&s->child, PROVIDER, &actions, NULL, argv, environ);
   posix_spawn_file_actions_destroy(&actions);
   close(pair[1]);
   if (rc)
   {
      close(pair[0]);
      s->child = 0;
      return -1;
   }
   s->channel = pair[0];
   int64_t now = milliseconds();
   int64_t end = now + 10000;
   if (s->deadline_ns && s->deadline_ns / 1000000 < (uint64_t)end)
      end = (int64_t)(s->deadline_ns / 1000000);
   unsigned char frame[4099];
   for (unsigned i = 0; i < 4; i++)
      frame[i] = (unsigned char)(s->local_policy >> (8 * i));
   size_t length = strlen(credential);
   memcpy(frame + 4, credential, length);
   int sent = now < 0 ? -1 : write_frame(s->channel, frame, (uint32_t)length + 4, end);
   volatile unsigned char *wipe = frame;
   for (size_t i = 0; i < sizeof(frame); i++)
      wipe[i] = 0;
   if (sent)
   {
      aimee_postgres_local_stop(s);
      return -1;
   }
   return 0;
}
int aimee_postgres_local_exchange(aimee_postgres_session_t *s, const void *request, uint32_t length,
                                  void *reply, uint32_t capacity, uint32_t *reply_length)
{
   int64_t now = milliseconds();
   if (s->channel < 0 || now < 0)
      return -1;
   int64_t deadline = now + 60000;
   if (s->deadline_ns && s->deadline_ns / 1000000 < (uint64_t)deadline)
      deadline = (int64_t)(s->deadline_ns / 1000000);
   unsigned char bytes[4];
   unsigned char header[12];
   if (length > UINT32_MAX - 8)
      goto failed;
   uint32_t frame_length = length + 8;
   for (unsigned i = 0; i < 4; i++)
      header[i] = (unsigned char)(frame_length >> (8 * i));
   uint64_t absolute = (uint64_t)deadline * 1000000;
   for (unsigned i = 0; i < 8; i++)
      header[4 + i] = (unsigned char)(absolute >> (8 * i));
   if (transfer(s->channel, header, sizeof(header), 1, deadline) ||
       transfer(s->channel, (void *)request, length, 1, deadline) ||
       transfer(s->channel, bytes, 4, 0, deadline))
      goto failed;
   uint32_t n = 0;
   for (unsigned i = 0; i < 4; i++)
      n |= (uint32_t)bytes[i] << (8 * i);
   if (!n || n > capacity || transfer(s->channel, reply, n, 0, deadline))
      goto failed;
   *reply_length = n;
   return 0;
failed:
   aimee_postgres_local_stop(s);
   return -1;
}
