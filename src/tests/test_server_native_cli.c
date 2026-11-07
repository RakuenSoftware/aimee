#include "server_native_cli.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static atomic_int closed;
static void *close_grant(void *p)
{
   server_native_cli_close(p);
   atomic_store(&closed, 1);
   return NULL;
}
static const char *env_value(char *const *env, const char *key)
{
   size_t n = strlen(key);
   for (char *const *p = env; p && *p; p++)
      if (!strncmp(*p, key, n) && (*p)[n] == '=')
         return *p + n + 1;
   return NULL;
}
static cJSON *request(const char *method)
{
   cJSON *r = cJSON_CreateObject();
   cJSON_AddStringToObject(r, "method", method);
   cJSON_AddStringToObject(r, "_cli_request_id", "0123456789abcdef0123456789abcdef");
   return r;
}
int main(void)
{
   char parent[75] = "native-ir:";
   memset(parent + 10, 'a', 64);
   parent[74] = 0;
   setenv("AIMEE_API_ENDPOINT", "unix:/must-not-inherit", 1);
   setenv("AIMEE_API_BEARER", "operator-secret-must-not-inherit", 1);
   setenv("AIMEE_SERVER_TOKEN", "operator-secret-must-not-inherit", 1);
   server_native_cli_t *g = server_native_cli_open("http://127.0.0.1:1234", "session-a",
                                                   "webuser:alice", "/owned/worktree", parent, 0);
   assert(g);
   const char *token = server_native_cli_token(g);
   assert(strlen(token) == 64 && server_native_cli_matches(g, token));
   assert(!server_native_cli_matches(g, "wrong"));
   assert(server_native_cli_owner(g, "session-a", "webuser:alice", "/owned/worktree"));
   assert(!server_native_cli_owner(g, "session-b", "webuser:alice", "/owned/worktree"));
   assert(!server_native_cli_owner(g, "session-a", "webuser:bob", "/owned/worktree"));
   assert(!server_native_cli_owner(g, "session-a", "webuser:alice", "/foreign/worktree"));
   char *const *env = server_native_cli_environment(g);
   assert(!strcmp(env_value(env, "AIMEE_API_BEARER"), token));
   assert(!strcmp(env_value(env, "AIMEE_API_ENDPOINT"), "http://127.0.0.1:1234"));
   assert(!strcmp(env_value(env, "AIMEE_SERVER_TOKEN"), ""));
   assert(!strcmp(getenv("AIMEE_API_BEARER"), "operator-secret-must-not-inherit"));
   assert(
       !server_native_cli_open("unix:/global/root.sock", "session-a", "alice", "/cwd", parent, 0));
   assert(
       !server_native_cli_open("http://192.168.0.1:1234", "session-a", "alice", "/cwd", parent, 0));
   assert(
       !server_native_cli_open("http://127.0.0.1:1234", "session-a", "alice", "/cwd", parent, 3));
   char tool[128], nonce[33], attempt[128];
   cJSON *args = NULL, *r = request("index.investigate"), *replay = NULL;
   cJSON_AddStringToObject(r, "query", "same registered project");
   assert(!server_native_cli_project(r, tool, &args, nonce));
   assert(!strcmp(tool, "index_investigate"));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(args, "query")),
                  "same registered project"));
   assert(!cJSON_GetObjectItem(args, "method") && !cJSON_GetObjectItem(args, "command"));
   cJSON_Delete(args);
   assert(!server_native_cli_begin(g, r, nonce, attempt, &replay));
   assert(!strncmp(attempt, "native-cli:aaaaaaaa", 19) && strlen(attempt) == 108);
   assert(server_native_cli_begin(g, r, nonce, attempt, &replay) ==
          -1); /* never queue behind self */
   cJSON *answer = cJSON_Parse("{\"hits\":[],\"status\":\"ok\"}");
   server_native_cli_finish(g, answer);
   assert(server_native_cli_begin(g, r, nonce, attempt, &replay) == 1);
   assert(cJSON_Compare(replay, answer, 1));
   cJSON_Delete(replay);
   cJSON_ReplaceItemInObject(r, "query", cJSON_CreateString("different operation"));
   assert(server_native_cli_begin(g, r, nonce, attempt, &replay) == -1);
   cJSON_Delete(r);
   cJSON_Delete(answer);
   r = request("provider.set");
   assert(server_native_cli_project(r, tool, &args, nonce) == -1);
   cJSON_Delete(r);
   r = request("index.not_an_advertised_command");
   assert(server_native_cli_project(r, tool, &args, nonce) == -1);
   cJSON_Delete(r);
   r = request("mcp.call");
   cJSON_AddStringToObject(r, "tool", "git_verify");
   cJSON_AddObjectToObject(r, "arguments");
   assert(!server_native_cli_project(r, tool, &args, nonce) && !strcmp(tool, "git_verify"));
   cJSON_Delete(args);
   cJSON_AddStringToObject(r, "_principal", "webuser:bob");
   assert(server_native_cli_project(r, tool, &args, nonce) == -1);
   cJSON_DeleteItemFromObject(r, "_principal");
   cJSON_AddStringToObject(r, "method", "provider.set");
   assert(server_native_cli_project(r, tool, &args, nonce) == -1);
   cJSON_Delete(r);
   r = request("mcp.call");
   cJSON_AddStringToObject(r, "tool", "read_file");
   cJSON *duplicate = cJSON_AddObjectToObject(r, "arguments");
   cJSON_AddNumberToObject(duplicate, "count", 1);
   cJSON_AddNumberToObject(duplicate, "count", 2);
   assert(server_native_cli_project(r, tool, &args, nonce) == -1);
   cJSON_Delete(r);
   r = request("mcp.call");
   cJSON_AddStringToObject(r, "tool", "read_file");
   cJSON_AddObjectToObject(r, "arguments");
   cJSON_AddStringToObject(r, "source", "forged-model-call");
   assert(server_native_cli_project(r, tool, &args, nonce) == -1);
   cJSON_DeleteItemFromObject(r, "source");
   cJSON_AddNullToObject(r, "cwd");
   assert(server_native_cli_project(r, tool, &args, nonce) == -1);
   cJSON_DeleteItemFromObject(r, "cwd");
   cJSON_AddNumberToObject(r, "session_id", 12);
   assert(server_native_cli_project(r, tool, &args, nonce) == -1);
   cJSON_Delete(r);
   r = request("background.run");
   cJSON_AddStringToObject(r, "command", "printf actual-command");
   cJSON_AddNumberToObject(r, "protocol_version", 1);
   assert(!server_native_cli_project(r, tool, &args, nonce));
   assert(!strcmp(tool, "run_background_process") &&
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(args, "command")),
                  "printf actual-command"));
   assert(!cJSON_GetObjectItem(args, "action") && !cJSON_GetObjectItem(args, "protocol_version"));
   cJSON_Delete(args);
   cJSON_AddStringToObject(r, "action", "kill");
   assert(server_native_cli_project(r, tool, &args, nonce) == -1);
   cJSON_DeleteItemFromObject(r, "action");
   cJSON_ReplaceItemInObject(r, "method", cJSON_CreateString("background.not_real"));
   assert(server_native_cli_project(r, tool, &args, nonce) == -1);
   cJSON_Delete(r);
   r = request("mcp.call");
   cJSON_AddStringToObject(r, "tool", "read_file");
   cJSON_AddObjectToObject(r, "arguments");
   cJSON_AddNumberToObject(r, "protocol_version", 1);
   assert(!server_native_cli_project(r, tool, &args, nonce));
   cJSON_Delete(args);
   cJSON_ReplaceItemInObject(r, "protocol_version", cJSON_CreateString("1"));
   assert(server_native_cli_project(r, tool, &args, nonce) == -1);
   cJSON_Delete(r);
   /* A consumed request with unknown/lost response cannot repeat effects. */
   r = request("index.investigate");
   cJSON_ReplaceItemInObject(r, "_cli_request_id",
                             cJSON_CreateString("abcdef0123456789abcdef0123456789"));
   assert(!server_native_cli_begin(g, r, "abcdef0123456789abcdef0123456789", attempt, &replay));
   server_native_cli_finish(g, NULL);
   assert(server_native_cli_begin(g, r, "abcdef0123456789abcdef0123456789", attempt, &replay) ==
          -1);
   cJSON_Delete(r);
   /* Complete response caching is bounded; overlarge/unknown outcomes are
    * consumed, never silently truncated or repeated. */
   r = request("index.investigate");
   cJSON_ReplaceItemInObject(r, "_cli_request_id",
                             cJSON_CreateString("eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"));
   assert(!server_native_cli_begin(g, r, "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee", attempt, &replay));
   char *large = malloc(1024u * 1024u + 1);
   assert(large);
   memset(large, 'x', 1024u * 1024u);
   large[1024u * 1024u] = 0;
   answer = cJSON_CreateObject();
   cJSON_AddStringToObject(answer, "content", large);
   free(large);
   assert(server_native_cli_finish(g, answer) == -1);
   cJSON_Delete(answer);
   assert(server_native_cli_begin(g, r, "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee", attempt, &replay) ==
          -1);
   cJSON_Delete(r);
   /* Owner returns while a callback is active: revoke first, join, then erase.
    * No callback must wait for a producer that is blocked in its shell. */
   assert(!server_native_cli_retain(g));
   pthread_t closer;
   assert(!pthread_create(&closer, NULL, close_grant, g));
   while (!server_native_cli_cancelled(g))
      usleep(1000);
   assert(!atomic_load(&closed));
   assert(!server_native_cli_matches(g, token));
   assert(server_native_cli_retain(g) == -1);
   server_native_cli_release(g);
   pthread_join(closer, NULL);
   assert(atomic_load(&closed));
   puts("server_native_cli: ok");
}
