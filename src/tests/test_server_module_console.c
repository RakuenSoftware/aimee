#define _GNU_SOURCE
#include "server_http_internal.h"
#include <aimee/ir/session_event.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef MODULE_CONSOLE_ABSENT_TEST
static int context_mode, response_mode, calls;
static pthread_mutex_t ready_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready_condition = PTHREAD_COND_INITIALIZER;
static int ready;

#endif
int err_json(char *out, int cap, int code, const char *message)
{
   snprintf(out, (size_t)cap, "{\"error\":\"%s\"}", message);
   return code;
}

#ifndef MODULE_CONSOLE_ABSENT_TEST
cJSON *aimee_module_host_command_context(void)
{
   if (context_mode == 1)
      return NULL;
   cJSON *context = cJSON_CreateObject();
   cJSON_AddBoolToObject(context, "authenticated", context_mode != 2);
   cJSON_AddStringToObject(context, "principal", "verified-human");
   if (context_mode == 3)
   {
      char text[5000];
      memset(text, 'x', sizeof text - 1);
      text[sizeof text - 1] = 0;
      cJSON_AddStringToObject(context, "transport_identity", text);
   }
   return context;
}
int aimee_module_console_admin_extension(const char *method, const char *path, const char *body,
                                         const char *context, char *out, int cap)
{
   calls++;
   assert(!strcmp(method, "POST"));
   assert(!strcmp(path, "/v1/console/memory"));
   assert(!strcmp(body, "{\"principal\":\"forged\",\"coordinate\":9007199254740993}"));
   cJSON *actual = cJSON_Parse(context);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(actual, "principal")),
                  "verified-human"));
   cJSON_Delete(actual);
   if (response_mode == 1)
   {
      memset(out, 'x', (size_t)cap);
      return 200;
   }
   if (response_mode == 2)
   {
      snprintf(out, (size_t)cap, "{} {}");
      return 200;
   }
   if (response_mode == 3)
   {
      snprintf(out, (size_t)cap, "{}");
      return -1;
   }
   if (response_mode == 4)
   {
      pthread_mutex_lock(&ready_lock);
      ready = 1;
      pthread_cond_signal(&ready_condition);
      pthread_mutex_unlock(&ready_lock);
      for (;;)
      {
         usleep(1000);
         pthread_testcancel();
      }
   }
   snprintf(out, (size_t)cap, "{\"status\":\"safe\"}");
   return 409;
}
#endif
static const char input[] = "{\"principal\":\"forged\",\"coordinate\":9007199254740993}";
static int run(const char *body, int length)
{
   route_req_t rq = {"POST", "/v1/console/memory", body, length, "", NULL};
   char out[1024];
   return rh_module_console_admin(&rq, out, sizeof out);
}
#ifndef MODULE_CONSOLE_ABSENT_TEST
static void *worker(void *unused)
{
   (void)unused;
   run(input, sizeof input - 1);
   return NULL;
}
#endif
int main(void)
{
#ifdef MODULE_CONSOLE_ABSENT_TEST
   assert(run(input, sizeof input - 1) == 503);
   puts("PASS absent optional console module refuses");
#else
   assert(run(input, sizeof input - 1) == 409 && calls == 1);
   const char *invalid[] = {"[]", "{} {}", "{", "null"};
   for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++)
      assert(run(invalid[i], (int)strlen(invalid[i])) == 400);
   assert(run("{}\0{}", 5) == 400);
   assert(run(NULL, 0) == 400);
   assert(calls == 1);
   context_mode = 1;
   assert(run(input, sizeof input - 1) == 403);
   context_mode = 2;
   assert(run(input, sizeof input - 1) == 403);
   context_mode = 3;
   assert(run(input, sizeof input - 1) == 503);
   assert(calls == 1);
   context_mode = 0;
   for (response_mode = 1; response_mode <= 3; response_mode++)
      assert(run(input, sizeof input - 1) == 503);
   response_mode = 4;
   pthread_t t;
   assert(!pthread_create(&t, NULL, worker, NULL));
   pthread_mutex_lock(&ready_lock);
   while (!ready)
      pthread_cond_wait(&ready_condition, &ready_lock);
   pthread_mutex_unlock(&ready_lock);
   assert(!pthread_cancel(t));
   void *result = NULL;
   assert(!pthread_join(t, &result));
   assert(result == PTHREAD_CANCELED);
   response_mode = 0;
   assert(run(input, sizeof input - 1) == 409);
   puts("PASS generic console HTTP: exact bytes/separate verified context, "
        "framing/auth/size/response refusal and cancellation");
#endif
}
