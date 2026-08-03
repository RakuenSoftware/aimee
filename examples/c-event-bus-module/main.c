#define _POSIX_C_SOURCE 200809L
#include <aimee/core/event_bus/bus_client.h>
#include <aimee/core/event_bus/bus_endpoint.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static uint32_t parse_identity(const char *text)
{
   char *end = NULL;
   errno = 0;
   unsigned long value = text ? strtoul(text, &end, 10) : 0;
   return !errno && end && !*end && value <= UINT32_MAX ? (uint32_t)value : UINT32_MAX;
}

int main(int argc, char **argv)
{
   if (argc != 4)
   {
      fprintf(stderr, "usage: %s ATTACH_SOCKET PRINCIPAL_CLASS PRINCIPAL_REF\n", argv[0]);
      return 2;
   }
   uint32_t principal_class = parse_identity(argv[2]);
   uint32_t principal_ref = parse_identity(argv[3]);
   if (principal_class == UINT32_MAX || principal_ref == UINT32_MAX)
      return 2;

   int socket_fd = -1;
   bus_client_t client;
   if (bus_endpoint_connect(argv[1], &socket_fd) != 0 ||
       bus_client_attach_as(socket_fd, &client, principal_class, principal_ref) != BUS_CLIENT_OK)
   {
      perror("event-bus attach");
      bus_endpoint_close(&socket_fd);
      return 1;
   }
   bus_endpoint_close(&socket_fd); /* shared memory is the data path after attach */

   for (;;)
   {
      struct timespec now;
      if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
         bus_client_heartbeat(&client, (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec);
      bus_event_t event;
      bus_client_result_t result = bus_client_poll(&client, &event);
      if (result == BUS_CLIENT_EPOCH)
         break;
      if (result == BUS_CLIENT_OK && (event.frame.hdr_flags & BUS_F_REQUEST))
         (void)bus_client_reply(&client, event.frame.event_kind, event.frame.correlation_id,
                                event.payload, event.payload_len);
      struct timespec idle = {.tv_sec = 0, .tv_nsec = 1000000};
      nanosleep(&idle, NULL);
   }
   bus_client_detach(&client);
   return 0;
}
