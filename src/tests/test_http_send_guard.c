/* Exercise the real socket boundary, including nested owner HTTP requests. */
#include "agent_exec.h"
#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct
{
   int listener;
   char url[128];
   pthread_t thread;
   atomic_int released;
   int guarded, refuse, acquired, release_count, write_status, delay_acquire;
   size_t received;
   const char *nested_url;
} peer_t;

static void *serve(void *opaque)
{
   peer_t *p = opaque;
   int fd = accept(p->listener, NULL, NULL);
   assert(fd >= 0);
   struct timeval timeout = {5, 0};
   assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
   char request[4096] = {0};
   for (;;)
   {
      ssize_t n = recv(fd, request + p->received, sizeof(request) - p->received - 1, 0);
      assert(n >= 0);
      if (!n)
         break;
      p->received += (size_t)n;
      if (strstr(request, "\r\n\r\n{}"))
         break;
      assert(p->received < sizeof(request) - 1);
   }
   if (p->refuse)
      assert(p->received == 0);
   else
   {
      assert(strstr(request, "\r\n\r\n{}"));
      /* Do not answer until release: holding a guard through the response
       * would deadlock here and fail the bounded client timeout. */
      for (int i = 0; p->guarded && !atomic_load(&p->released) && i < 5000; ++i)
         usleep(1000);
      assert(!p->guarded || atomic_load(&p->released));
      const char response[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
      assert(send(fd, response, sizeof(response) - 1, 0) == sizeof(response) - 1);
   }
   close(fd);
   return NULL;
}

static void start(peer_t *p)
{
   atomic_init(&p->released, 0);
   p->listener = socket(AF_INET, SOCK_STREAM, 0);
   assert(p->listener >= 0);
   struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
   assert(bind(p->listener, (struct sockaddr *)&address, sizeof(address)) == 0);
   assert(listen(p->listener, 1) == 0);
   socklen_t length = sizeof(address);
   assert(getsockname(p->listener, (struct sockaddr *)&address, &length) == 0);
   snprintf(p->url, sizeof(p->url), "http://127.0.0.1:%u/test", ntohs(address.sin_port));
   assert(pthread_create(&p->thread, NULL, serve, p) == 0);
}

static void finish(peer_t *p)
{
   assert(pthread_join(p->thread, NULL) == 0);
   close(p->listener);
}

static int acquire(void *opaque)
{
   peer_t *p = opaque;
   ++p->acquired;
   if (p->delay_acquire)
   {
      usleep(50000);
      return 0;
   }
   if (p->nested_url)
   {
      char *response = NULL;
      assert(agent_http_post(p->nested_url, NULL, "{}", &response, 2000, NULL) == 200);
      assert(response && strcmp(response, "{}") == 0);
      free(response);
   }
   return p->refuse ? -1 : 0;
}

static void release(void *opaque, int status)
{
   peer_t *p = opaque;
   ++p->release_count;
   p->write_status = status;
   atomic_store(&p->released, 1);
}

static int consume(const char *data, size_t size, void *opaque)
{
   (void)data;
   *(size_t *)opaque += size;
   return 0;
}

int main(void)
{
   agent_http_init();
   for (int stream = 0; stream < 2; ++stream)
      for (int scenario = 0; scenario < 3; ++scenario)
      {
         int refuse = scenario != 0;
         peer_t nested = {0};
         if (scenario != 2)
            start(&nested);
         peer_t provider = {.guarded = 1,
                            .refuse = refuse,
                            .nested_url = nested.url,
                            .delay_acquire = scenario == 2};
         start(&provider);
         agent_http_send_guard_t guard = {.context = &provider,
                                          .acquire = acquire,
                                          .release = release,
                                          .send_timeout_ms = scenario == 2 ? 10 : 2000};
         char *response = NULL;
         size_t received = 0;
         int status =
             stream ? agent_http_post_stream_guarded_bytes(provider.url, NULL, "{}", 2, consume,
                                                           &received, 3000, NULL, &guard)
                    : agent_http_post_guarded_bytes(provider.url, NULL, "{}", 2, &response, 3000,
                                                    NULL, &guard);
         assert(status == (refuse ? -2 : 200));
         assert(provider.acquired == 1 && provider.release_count == 1);
         assert(provider.write_status == (refuse ? -2 : 0));
         if (!refuse)
            assert(stream ? received == 2 : response && strcmp(response, "{}") == 0);
         free(response);
         finish(&provider);
         if (scenario != 2)
         {
            finish(&nested);
            assert(nested.received > 0);
         }
      }
   agent_http_cleanup();
   puts("http_send_guard: buffered/streaming refusal, release-before-response and nested calls "
        "pass");
   return 0;
}
