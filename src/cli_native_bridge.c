/* Explicit CLI bearer resolution and the native shell's bounded transport.
 * Native child mode consumes an already marshalled command and never falls
 * back to global credentials, remote.conf or operator routes. */
#include "cli_v1_routes_internal.h"
#include "cli_client.h"
#include "cJSON.h"
#include "aimee_client.h"
#include "platform_random.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cli_native_bridge_enabled(void)
{
   const char *mode = getenv("AIMEE_NATIVE_CLI");
   return mode && !strcmp(mode, "1");
}

/* cli_v1_client_bearer: bearer token sent with the remote HTTP transport.
 * AIMEE_API_BEARER overrides; otherwise use the explicit remote credential.
 * Credentials are never read from public config storage. Caller frees. */
char *cli_v1_client_bearer(void)
{
   const char *env = getenv("AIMEE_API_BEARER");
   if (env && env[0])
      return strdup(env);
   if (cli_native_bridge_enabled())
      return NULL;
   /* Token for a synthesized aimee_client endpoint: --server-token /
    * AIMEE_SERVER_TOKEN / remote.conf line 2. */
   char tok[300];
   if (aimee_client_remote_token(tok, sizeof(tok)) && tok[0])
      return strdup(tok);
   return NULL;
}

/* The host generates this exact loopback form. Unlike ordinary CLI endpoint
 * resolution, native child mode cannot fall back to remote.conf/global auth. */
char *cli_native_bridge_endpoint(void)
{
   const char *value = getenv("AIMEE_API_ENDPOINT");
   const char *prefix = "http://127.0.0.1:";
   size_t n = strlen(prefix);
   if (!value || strncmp(value, prefix, n) || !value[n])
      return NULL;
   unsigned port = 0;
   for (const char *p = value + n; *p; p++)
   {
      if (*p < '0' || *p > '9' || port > 6553)
         return NULL;
      port = port * 10 + (unsigned)(*p - '0');
   }
   if (!port || port > 65535)
      return NULL;
   char endpoint[64];
   snprintf(endpoint, sizeof endpoint, "tcp:127.0.0.1:%u", port);
   return strdup(endpoint);
}

static cJSON *cli_native_bridge_request(cJSON *req, int *http_status, int *tool_failed)
{
   *http_status = 0;
   *tool_failed = 1;
   unsigned char bytes[16];
   char nonce[33] = "";
   if (platform_random_bytes(bytes, sizeof bytes))
   {
      cJSON_Delete(req);
      return NULL;
   }
   for (unsigned i = 0; i < sizeof bytes; i++)
      snprintf(nonce + i * 2, 3, "%02x", bytes[i]);
   /* A marshaller cannot choose a different host request identity. */
   while (cJSON_GetObjectItemCaseSensitive(req, "_cli_request_id"))
      cJSON_DeleteItemFromObjectCaseSensitive(req, "_cli_request_id");
   char *endpoint = cli_native_bridge_endpoint();
   const char *supplied = getenv("AIMEE_API_BEARER");
   char *credential = supplied && supplied[0] ? strdup(supplied) : NULL;
   int valid = endpoint && credential && cJSON_AddStringToObject(req, "_cli_request_id", nonce);
   char *wire = valid ? cJSON_PrintUnformatted(req) : NULL;
   int status = 0;
   cJSON *response = wire ? cli_http_request(endpoint, "POST", "/v1/native/cli", wire, credential,
                                             120000, &status)
                          : NULL;
   free(wire);
   free(endpoint);
   free(credential);
   cJSON_Delete(req);
   const cJSON *content = cJSON_GetObjectItemCaseSensitive(response, "content");
   const cJSON *error = cJSON_GetObjectItemCaseSensitive(response, "isError");
   int complete = cJSON_IsArray(content) && cJSON_GetArraySize(content) > 0 && cJSON_IsBool(error);
   const cJSON *item;
   cJSON_ArrayForEach(item, content)
   {
      const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "type"));
      complete &= type && !strcmp(type, "text") &&
                  cJSON_IsString(cJSON_GetObjectItemCaseSensitive(item, "text"));
   }
   if (!complete)
   {
      cJSON_Delete(response);
      fprintf(stderr, "aimee: native CLI tool receipt unavailable or malformed\n");
      return NULL;
   }
   *tool_failed = cJSON_IsTrue(error);
   *http_status = status;
   return response;
}

int cli_native_bridge_forward(cJSON *request, int json_output, int argc, char **argv,
                              cli_native_finish_fn finish)
{
   int status = 0, tool_failed = 1;
   cJSON *response = cli_native_bridge_request(request, &status, &tool_failed);
   if (!response)
      return 1;
   /* Native-only output is the existing exact MCP content/isError contract,
    * rather than specialized operator REST batch-envelope presentation. */
   const cli_v1_route_t tool_route = {.method = "tool.call"};
   int result = finish(&tool_route, response, status, json_output, argc, argv);
   return tool_failed ? 1 : result;
}

/* Native main deliberately has no operator bootstrap or local-command path.
 * The ordinary served catalogue and marshaller still select every command. */
int cli_native_bridge_main(int argc, char **argv)
{
   int json = 0, start = 1;
   const char *fields = NULL;
   for (; start < argc; start++)
   {
      if (!strcmp(argv[start], "--json"))
         json = 1;
      else if (!strcmp(argv[start], "--debug"))
         continue;
      else if (!strncmp(argv[start], "--fields=", 9))
         fields = argv[start] + 9;
      else
         break;
   }
   char *endpoint = cli_native_bridge_endpoint();
   const char *bearer = getenv("AIMEE_API_BEARER");
   int complete = endpoint && bearer && bearer[0];
   free(endpoint);
   if (!complete || start >= argc)
   {
      fprintf(stderr, "aimee: native CLI capability is unavailable or command is missing\n");
      return 1;
   }
   cli_v1_route_t route;
   int sub_argc = argc - start - 1;
   char **sub_argv = argv + start + 1;
   if (!cli_v1_lookup(argv[start], sub_argc, sub_argv, &route))
   {
      fprintf(stderr, "aimee: command is unavailable in the native CLI catalogue\n");
      return 1;
   }
   int rc = cli_v1_forward(NULL, &route, json, fields, NULL, sub_argc, sub_argv);
   return rc >= 0 ? rc : 1;
}
