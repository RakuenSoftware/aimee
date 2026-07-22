#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_http_resolver_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int read_exact(unsigned char *buffer, size_t length)
{
   size_t at = 0;
   while (at < length)
   {
      ssize_t n = read(STDIN_FILENO, buffer + at, length - at);
      if (n > 0)
         at += (size_t)n;
      else if (n == 0)
         return -1;
      else if (errno != EINTR)
         return -1;
   }
   return 0;
}

static int write_exact(const unsigned char *buffer, size_t length)
{
   size_t at = 0;
   while (at < length)
   {
      ssize_t n = write(STDOUT_FILENO, buffer + at, length - at);
      if (n > 0)
         at += (size_t)n;
      else if (n < 0 && errno == EINTR)
         continue;
      else
         return -1;
   }
   return 0;
}

static int protocol_failure(void)
{
   unsigned char response[8] = {0};
   kb_resolver_put_u32(response, KB_RESOLVER_RESPONSE_MAGIC);
   response[4] = KB_RESOLVER_VERSION;
   response[5] = KB_RESOLVER_STATUS_PROTOCOL;
   return write_exact(response, sizeof(response)) == 0 ? 2 : 3;
}

int main(void)
{
   unsigned char header[8];
   if (read_exact(header, sizeof(header)) != 0 ||
       kb_resolver_get_u32(header) != KB_RESOLVER_REQUEST_MAGIC ||
       header[4] != KB_RESOLVER_VERSION || (header[5] & ~KB_RESOLVER_FLAG_HANG_TEST) != 0 ||
       !header[6] || !header[7])
      return protocol_failure();
   size_t host_len = header[6], service_len = header[7];
   char host[KB_RESOLVER_HOST_MAX + 1U], service[KB_RESOLVER_SERVICE_MAX + 1U];
   if (host_len > KB_RESOLVER_HOST_MAX || service_len > KB_RESOLVER_SERVICE_MAX ||
       read_exact((unsigned char *)host, host_len) != 0 ||
       read_exact((unsigned char *)service, service_len) != 0)
      return protocol_failure();
   host[host_len] = 0;
   service[service_len] = 0;
   unsigned char trailing;
   ssize_t extra;
   do
      extra = read(STDIN_FILENO, &trailing, 1);
   while (extra < 0 && errno == EINTR);
   if (extra != 0 || memchr(host, 0, host_len) || memchr(service, 0, service_len))
      return protocol_failure();
   if (header[5] == KB_RESOLVER_FLAG_HANG_TEST)
      for (;;)
         pause();

   struct addrinfo hints = {
       .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_protocol = IPPROTO_TCP};
   struct addrinfo *addresses = NULL;
   int gai = getaddrinfo(host, service, &hints, &addresses);
   unsigned char response[KB_RESOLVER_WIRE_MAX] = {0};
   kb_resolver_put_u32(response, KB_RESOLVER_RESPONSE_MAGIC);
   response[4] = KB_RESOLVER_VERSION;
   response[5] = gai == 0 ? KB_RESOLVER_STATUS_OK : KB_RESOLVER_STATUS_NOT_FOUND;
   size_t at = 8, count = 0;
   if (gai == 0)
   {
      for (struct addrinfo *a = addresses; a && count < KB_RESOLVER_RECORD_MAX; a = a->ai_next)
      {
         const unsigned char *address = NULL;
         size_t address_len = 0;
         uint16_t port = 0;
         unsigned family = 0;
         if (a->ai_family == AF_INET && a->ai_addrlen >= sizeof(struct sockaddr_in))
         {
            const struct sockaddr_in *in = (const struct sockaddr_in *)a->ai_addr;
            family = 4;
            address = (const unsigned char *)&in->sin_addr;
            address_len = 4;
            port = ntohs(in->sin_port);
         }
         else if (a->ai_family == AF_INET6 && a->ai_addrlen >= sizeof(struct sockaddr_in6))
         {
            const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)a->ai_addr;
            family = 6;
            address = (const unsigned char *)&in6->sin6_addr;
            address_len = 16;
            port = ntohs(in6->sin6_port);
         }
         else
            continue;
         if (at + 5U + address_len > sizeof(response))
            break;
         response[at++] = (unsigned char)family;
         response[at++] = (unsigned char)address_len;
         kb_resolver_put_u16(response + at, port);
         at += 2;
         response[at++] = 0;
         memcpy(response + at, address, address_len);
         at += address_len;
         count++;
      }
      if (!count)
         response[5] = KB_RESOLVER_STATUS_NOT_FOUND;
   }
   response[6] = (unsigned char)count;
   if (addresses)
      freeaddrinfo(addresses);
   return write_exact(response, at) == 0 ? 0 : 3;
}
