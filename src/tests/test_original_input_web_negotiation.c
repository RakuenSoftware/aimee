#include "server/host_original_submission.h"
#include "server_http_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
static int owner_calls;
const char *ingress_preinject_turn_id(void)
{
   return NULL;
}
int anthropic_http_response_retry_after(void)
{
   return 0;
}
int write_all_fd(int fd, const char *bytes, int length)
{
   int sent = 0;
   while (sent < length)
   {
      ssize_t n = write(fd, bytes + sent, (size_t)(length - sent));
      if (n <= 0)
         return -1;
      sent += (int)n;
   }
   return 0;
}
static host_text_status capture(const char *h, const struct cJSON *r, void **o, void *s)
{
   (void)h;
   (void)r;
   (void)o;
   (void)s;
   owner_calls++;
   return HOST_TEXT_REFUSED;
}
static host_text_status consume(void *o, const struct cJSON *r,
                                host_original_submission_result *out, void *s)
{
   (void)o;
   (void)r;
   (void)out;
   (void)s;
   owner_calls++;
   return HOST_TEXT_REFUSED;
}
static void finish(void *o, int admitted, void *s)
{
   (void)o;
   (void)admitted;
   (void)s;
   owner_calls++;
}
static void destroy(void *o, void *s)
{
   (void)o;
   (void)s;
   owner_calls++;
}
static void response(const char *expected)
{
   int fd[2];
   assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
   send_response(fd[0], 200, "{}", NULL);
   close(fd[0]);
   char wire[4096];
   size_t used = 0;
   for (;;)
   {
      ssize_t n = read(fd[1], wire + used, sizeof wire - used - 1);
      if (n <= 0)
         break;
      used += (size_t)n;
      assert(used < sizeof wire - 1);
   }
   close(fd[1]);
   wire[used] = 0;
   char header[100];
   snprintf(header, sizeof header, "\r\nX-Aimee-Original-Input: %s\r\n", expected);
   const char *found = strstr(wire, header);
   assert(found);
   assert(!strstr(found + strlen(header), "X-Aimee-Original-Input:"));
   assert(strstr(wire, "Content-Length: 2\r\n") && strstr(wire, "\r\n\r\n{}"));
   assert(!owner_calls);
}
int main(void)
{
   assert(!host_original_submission_required());
   response("optional");
   host_original_submission_owner owner = {capture, consume, finish, destroy};
   assert(!host_original_submission_install(&owner, NULL));
   assert(host_original_submission_required() == 1);
   response("required");
   assert(host_original_submission_required() == 1 && !owner_calls);
   puts("native original-input readiness: actual optional/required HTTP headers, no source "
        "callbacks or authority minting");
   return 0;
}
