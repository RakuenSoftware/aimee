#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aimee.h"
#include "cJSON.h"
#include "commands.h"
#include "config_client.h"
#include "platform_path.h"
#include "platform_test_util.h"

static char *capture_stdout(void (*fn)(void *), void *arg)
{
   int pipefd[2];
   assert(pipe(pipefd) == 0);

   fflush(stdout);
   int saved_stdout = dup(STDOUT_FILENO);
   assert(saved_stdout >= 0);
   assert(dup2(pipefd[1], STDOUT_FILENO) >= 0);
   close(pipefd[1]);

   fn(arg);

   fflush(stdout);
   assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
   close(saved_stdout);

   char buf[4096];
   ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
   assert(n >= 0);
   close(pipefd[0]);
   buf[n] = '\0';
   return strdup(buf);
}

typedef struct
{
   int argc;
   char **argv;
} cmd_call_t;

static void run_cmd_config(void *arg)
{
   cmd_call_t *call = (cmd_call_t *)arg;
   cmd_config(NULL, call->argc, call->argv);
}

static void test_dispositions_text_and_json(void)
{
   cJSON *rows = cJSON_CreateArray();
   cJSON *skepticism = cJSON_CreateObject();
   cJSON_AddStringToObject(skepticism, "name", "skepticism");
   cJSON_AddNumberToObject(skepticism, "value", 0.2);
   cJSON_AddNumberToObject(skepticism, "source", CONFIG_DISPOSITION_SOURCE_PROJECT);
   cJSON_AddItemToArray(rows, skepticism);
   cJSON *empathy = cJSON_CreateObject();
   cJSON_AddStringToObject(empathy, "name", "empathy");
   cJSON_AddNumberToObject(empathy, "value", 0.6);
   cJSON_AddNumberToObject(empathy, "source", CONFIG_DISPOSITION_SOURCE_WORKSPACE);
   cJSON_AddItemToArray(rows, empathy);
   assert(config_client_set_value("dispositions", rows) == 0);
   assert(config_client_set_number("disposition_count", 2) == 0);

   char *argv_text[] = {"dispositions"};
   cmd_call_t text_call = {.argc = 1, .argv = argv_text};
   char *text = capture_stdout(run_cmd_config, &text_call);
   assert(text);
   assert(strstr(text, "skepticism\t0.20\tproject") != NULL);
   assert(strstr(text, "empathy\t0.60\tworkspace") != NULL);
   free(text);

   char *argv_json[] = {"dispositions", "--json"};
   cmd_call_t json_call = {.argc = 2, .argv = argv_json};
   char *json = capture_stdout(run_cmd_config, &json_call);
   assert(json);
   assert(strstr(json, "\"name\":\"skepticism\"") != NULL);
   assert(strstr(json, "\"source\":\"project\"") != NULL);
   assert(strstr(json, "\"source\":\"workspace\"") != NULL);
   free(json);
}

int main(void)
{
   printf("cmd_config: ");
   test_dispositions_text_and_json();
   assert(config_client_key_is_secret("kb_api_bearer_token"));
   assert(strcmp(config_client_secret_name("kb_api_bearer_token"), "AIMEE_KB_API_BEARER_TOKEN") ==
          0);
   assert(!config_client_key_is_secret("provider"));
   printf("OK\n");
   return 0;
}
