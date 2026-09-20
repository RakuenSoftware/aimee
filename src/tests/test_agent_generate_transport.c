/* Exercise the real CLI adapter while keeping provider execution deterministic. */
#include "../cmd_agent.c"
#include <assert.h>
#include <stdarg.h>
#include <sys/wait.h>

static int expect_model;
static int completions;
static int emitted;

void agent_http_init(void)
{
}
void agent_http_cleanup(void)
{
}

int agent_generate(agent_config_t *cfg, const char *name, const char *system, const char *prompt,
                   int max_tokens, double temperature, agent_result_t *out)
{
   assert(expect_model && cfg == &s_agent_cfg && name == NULL);
   assert(!strcmp(system, "test system"));
   assert(strlen(prompt) > 20000 && strstr(prompt, "$(exit 9); `exit 8`"));
   assert(max_tokens == 64 && temperature == 0);
   memset(out, 0, sizeof(*out));
   out->response = strdup("Orion");
   out->prompt_tokens = 7;
   out->completion_tokens = 2;
   completions++;
   return 0;
}

void emit_json_ctx(cJSON *json, const char *fields, const char *profile)
{
   (void)fields;
   (void)profile;
   assert(!strcmp(cJSON_GetObjectItemCaseSensitive(json, "response")->valuestring, "Orion"));
   assert(cJSON_GetObjectItemCaseSensitive(json, "prompt_tokens")->valueint == 7);
   emitted++;
   cJSON_Delete(json);
}

void fatal(const char *fmt, ...)
{
   (void)fmt;
   _exit(73);
}

static void invoke(const char *request)
{
   FILE *input = tmpfile();
   assert(input);
   assert(fwrite(request, 1, strlen(request), input) == strlen(request));
   rewind(input);
   FILE *previous = stdin;
   stdin = input;
   app_ctx_t ctx = {.json_output = 1};
   ag_generate(&ctx, 0, NULL);
   stdin = previous;
   fclose(input);
}

int main(void)
{
   char *prompt = malloc(30000);
   assert(prompt);
   memset(prompt, 'x', 25000);
   strcpy(prompt + 25000, "$(exit 9); `exit 8` é文");
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "system", "test system");
   cJSON_AddStringToObject(request, "prompt", prompt);
   cJSON_AddNumberToObject(request, "max_tokens", 64);
   cJSON_AddNumberToObject(request, "temperature", 0);
   char *raw = cJSON_PrintUnformatted(request);
   expect_model = 1;
   invoke(raw);
   assert(completions == 1 && emitted == 1);
   free(raw);
   free(prompt);
   cJSON_Delete(request);
   expect_model = 0;
   const char *bad[] = {"{}",
                        "null",
                        "{} trailing",
                        "{\"prompt\":\"x\",\"system\":\"s\",\"max_tokens\":0,\"temperature\":0}",
                        "{\"prompt\":\"x\",\"system\":\"s\",\"max_tokens\":64.5,\"temperature\":0}",
                        "{\"prompt\":\"x\",\"system\":\"s\",\"max_tokens\":64,\"temperature\":3}"};
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
   {
      pid_t child = fork();
      assert(child >= 0);
      if (!child)
      {
         invoke(bad[i]);
         _exit(0);
      }
      int status;
      assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 73);
   }
   puts("agent generate: tool-free completion transport passed");
   return 0;
}
