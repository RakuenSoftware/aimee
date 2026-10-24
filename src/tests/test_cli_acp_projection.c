/* Actual ACP CLI callbacks and serializer, without running CLI dispatch/model I/O. */
#define main cli_application_main
#include "../cli_main.c"
#undef main
#include <assert.h>
#include <aimee/ir/session_event.h>
#ifndef ACP_OLD_PROJECTION_PROBE
static char *legacy_large_dispatch(const char *text, const char *session)
{
   assert(!strcmp(text, "large") && !strcmp(session, "editor"));
   char *out = malloc(100001);
   assert(out);
   memset(out, 'x', 100000);
   out[100000] = 0;
   return out;
}
static void legacy_large_run_loop(void)
{
   FILE *in = tmpfile(), *out = tmpfile();
   assert(in && out);
   fputs("{\"jsonrpc\":\"2.0\",\"method\":\"prompt/"
         "submit\",\"params\":{\"content\":\"large\",\"sessionId\":\"editor\"},\"id\":7}\n",
         in);
   rewind(in);
   int old_in = dup(STDIN_FILENO), old_out = dup(STDOUT_FILENO);
   assert(old_in >= 0 && old_out >= 0);
   fflush(stdout);
   assert(dup2(fileno(in), STDIN_FILENO) >= 0 && dup2(fileno(out), STDOUT_FILENO) >= 0);
   clearerr(stdin);
   assert(!acp_server_run(legacy_large_dispatch));
   fflush(stdout);
   assert(dup2(old_in, STDIN_FILENO) >= 0 && dup2(old_out, STDOUT_FILENO) >= 0);
   close(old_in);
   close(old_out);
   clearerr(stdin);
   rewind(out);
   char *line = NULL;
   size_t cap = 0;
   assert(getline(&line, &cap, out) > 100000);
   cJSON *j = aimee_ir_json_parse_frame(line, strlen(line));
   assert(j);
   const char *content =
       cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "result"), "content"));
   assert(content && strlen(content) == 100000 && content[0] == 'x' && content[99999] == 'x');
   assert(cJSON_GetObjectItem(j, "id")->valueint == 7);
   cJSON_Delete(j);
   free(line);
   fclose(in);
   fclose(out);
}
#endif

int main(void)
{
   setvbuf(stdout, NULL, _IONBF, 0);
#ifndef ACP_OLD_PROJECTION_PROBE
   legacy_large_run_loop();
#endif
   char text[18001], id[1201], name[1025];
   for (size_t i = 0; i < sizeof text - 1; i++)
      text[i] = i % 2 ? '"' : '\\';
   text[sizeof text - 1] = 0;
   memset(id, 'i', sizeof id - 1);
   id[sizeof id - 1] = 0;
   memset(name, 'n', sizeof name - 1);
   name[sizeof name - 1] = 0;
   FILE *capture = tmpfile();
   assert(capture);
   int saved = dup(STDOUT_FILENO);
   assert(saved >= 0);
   fflush(stdout);
   assert(dup2(fileno(capture), STDOUT_FILENO) >= 0);
#ifdef ACP_OLD_PROJECTION_PROBE
   acp_stream_delta(text, "editor-session");
#else
   acp_stream_context_t context = {.session = "editor-session"};
   acp_stream_delta(text, &context);
   acp_stream_tool("started", id, name, &context);
   acp_stream_tool("failed", id, name, &context);
   assert(!context.failed);
#endif
   fflush(stdout);
   assert(dup2(saved, STDOUT_FILENO) >= 0);
   close(saved);
   rewind(capture);
   char *line = NULL;
   size_t cap = 0;
   int count = 0;
   while (getline(&line, &cap, capture) > 0)
   {
      cJSON *j = aimee_ir_json_parse_frame(line, strlen(line));
      assert(j);
      cJSON *update = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "params"), "update");
      if (!count)
      {
         cJSON *content = cJSON_GetObjectItem(update, "content");
         assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(content, "text")), text));
      }
      else
      {
         assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(update, "toolCallId")), id));
         assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(update, "status")),
                        count == 1 ? "in_progress" : "failed"));
      }
      count++;
      cJSON_Delete(j);
   }
#ifdef ACP_OLD_PROJECTION_PROBE
   assert(count == 1);
#else
   assert(count == 3);
#endif
   free(line);
   fclose(capture);
   char tiny[8] = "stale";
   assert(acp_build_update_notification("editor", text, tiny, sizeof tiny) < 0 && !tiny[0]);
#ifndef ACP_OLD_PROJECTION_PROBE
   FILE *full = fopen("/dev/full", "w");
   assert(full);
   saved = dup(STDOUT_FILENO);
   assert(saved >= 0);
   assert(dup2(fileno(full), STDOUT_FILENO) >= 0);
   context.failed = 0;
   acp_stream_delta("cannot write", &context);
   assert(context.failed);
   clearerr(stdout);
   assert(dup2(saved, STDOUT_FILENO) >= 0);
   close(saved);
   fclose(full);
#endif
   puts("ACP exact projection: large TEXT/opaque IDs, failed tool, buffer and output failures "
        "passed");
   return 0;
}
