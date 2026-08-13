#define _GNU_SOURCE
#include <aimee/core/event_bus/bus_endpoint.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

int main(void)
{
   char directory[256];
   snprintf(directory, sizeof directory, "%s/aimee-bus-endpoint-XXXXXX", platform_tmpdir());
   assert(mkdtemp(directory) != NULL);
   char path[256];
   assert(snprintf(path, sizeof(path), "%s/attach.sock", directory) > 0);

   int listener = -1, client = -1, server = -1;
   assert(bus_endpoint_listen(path, 0600, 4, &listener) == 0);
   struct stat status;
   assert(lstat(path, &status) == 0 && S_ISSOCK(status.st_mode));
   assert((status.st_mode & 0777) == 0600);
   int duplicate = -1;
   assert(bus_endpoint_listen(path, 0600, 4, &duplicate) == -1 && duplicate == -1);
   assert(lstat(path, &status) == 0 && S_ISSOCK(status.st_mode));
   assert(bus_endpoint_connect(path, &client) == 0);
   assert(bus_endpoint_accept(listener, &server) == 0);

   static const char message[] = "attach";
   char received[sizeof(message)] = {0};
   assert(send(client, message, sizeof(message), 0) == (ssize_t)sizeof(message));
   assert(recv(server, received, sizeof(received), 0) == (ssize_t)sizeof(received));
   assert(memcmp(message, received, sizeof(message)) == 0);

   assert(bus_endpoint_close(&server) == 0 && server == -1);
   assert(bus_endpoint_close(&client) == 0 && client == -1);
   assert(bus_endpoint_close(&listener) == 0 && listener == -1);
   assert(bus_endpoint_remove(path) == 0);
   assert(rmdir(directory) == 0);
   puts("bus endpoint: local attach socket passed");
   return 0;
}
