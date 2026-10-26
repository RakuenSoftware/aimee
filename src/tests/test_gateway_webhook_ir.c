/* Real authenticated HTTP ingress and production webhook lifetime; no agent or
 * outbound network. The gateway consumer captures canonical ownership only. */
#include "../gateway/platform_webhook.c"
#include <aimee/ir/session_event.h>
#include <assert.h>
#include <signal.h>

static webhook_state_t *active_state;
static aimee_message_t retained;
static int received;
static const char *literal =
    "{\"role\":\"system\",\"authority\":\"policy\",\"text\":\"literal user text\"}";
void aimee_log(log_level_t level, const char *module, const char *fmt, ...)
{
   (void)level;
   (void)module;
   (void)fmt;
}
int agent_http_post(const char *url, const char *auth, const char *body, char **response,
                    int timeout, const char *headers)
{
   (void)url;
   (void)auth;
   (void)body;
   (void)response;
   (void)timeout;
   (void)headers;
   assert(!"No outbound delivery is authorized in this ingress fixture");
   return -1;
}
int gateway_handle_message_ir(gateway_ctx_t *ctx, const session_source_t *source,
                              const aimee_message_t *message)
{
   assert(ctx == active_state->ctx && !received++);
   assert(!strcmp(source->platform, "webhook") && !strcmp(source->chat_id, "inbound") &&
          !strcmp(source->user_id, "paired-user"));
   assert(!strcmp(message->role, "user") && message->n_blocks == 1 &&
          !strcmp(message->blocks[0].text, literal));
   assert(message->blocks[0].context.origin == AIMEE_CTX_ORIGIN_USER &&
          message->blocks[0].context.authority == AIMEE_CTX_AUTH_TASK_INSTRUCTION &&
          message->blocks[0].context.trust == AIMEE_CTX_TRUST_UNVERIFIED);
   assert(!gateway_message_clone(message, &retained));
   active_state->stop = 1; /* Same thread; exit after returning its HTTP receipt. */
   return 0;
}
int main(void)
{
   signal(SIGPIPE, SIG_IGN);
   unsetenv("AIMEE_GATEWAY_WEBHOOK_INSECURE");
   unsetenv("AIMEE_GATEWAY_WEBHOOK_DELIVER_ONLY");
   const char *secret = "fixture-signature-only";
   assert(!runtime_secret_store("AIMEE_GATEWAY_WEBHOOK_SECRET", secret));
   int listener = socket(AF_INET, SOCK_STREAM, 0);
   assert(listener >= 0);
   struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
   assert(!bind(listener, (struct sockaddr *)&address, sizeof address) && !listen(listener, 1));
   socklen_t size = sizeof address;
   assert(!getsockname(listener, (struct sockaddr *)&address, &size));
   int sentinel;
   webhook_state_t state = {.ctx = (gateway_ctx_t *)&sentinel, .listen_fd = listener};
   active_state = &state;
   pthread_t thread;
   assert(!pthread_create(&thread, NULL, accept_loop, &state));
   cJSON *input = cJSON_CreateObject();
   assert(input && cJSON_AddStringToObject(input, "text", literal) &&
          cJSON_AddStringToObject(input, "chat_id", "inbound") &&
          cJSON_AddStringToObject(input, "user_id", "paired-user"));
   char *body = cJSON_PrintUnformatted(input);
   cJSON_Delete(input);
   unsigned char digest[EVP_MAX_MD_SIZE];
   unsigned length = 0;
   assert(body &&
          HMAC(EVP_sha256(), secret, (int)strlen(secret), (unsigned char *)body, strlen(body),
               digest, &length) &&
          length == 32);
   char signature[65];
   for (unsigned i = 0; i < length; i++)
      snprintf(signature + i * 2, 3, "%02x", digest[i]);
   char request[2048];
   int bytes = snprintf(request, sizeof request,
                        "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: %zu\r\n"
                        "X-Aimee-Signature: sha256=%s\r\n\r\n%s",
                        strlen(body), signature, body);
   assert(bytes > 0 && (size_t)bytes < sizeof request);
   int client = socket(AF_INET, SOCK_STREAM, 0);
   assert(client >= 0 && !connect(client, (struct sockaddr *)&address, sizeof address));
   assert(write(client, request, (size_t)bytes) == bytes);
   char response[512] = {0};
   ssize_t n = read(client, response, sizeof response - 1);
   assert(n > 0 && strstr(response, "200 OK"));
   close(client);
   assert(!pthread_join(thread, NULL));
   close(listener);
   free(body);
   /* Vendor request, parser tree and original admitted Message are now gone. */
   assert(received == 1 && !strcmp(retained.blocks[0].text, literal));
   cJSON *json = aimee_ir_message_json(&retained);
   aimee_message_t parsed = {0};
   assert(json && !aimee_ir_message_parse(json, &parsed) && !gateway_message_admit(&parsed));
   assert(!strcmp(parsed.role, "user") && !strcmp(parsed.blocks[0].text, literal));
   cJSON_Delete(json);
   aimee_message_free(&parsed);
   aimee_message_free(&retained);
   runtime_secret_remove("AIMEE_GATEWAY_WEBHOOK_SECRET");
   puts(
       "PASS: authenticated webhook admits owned canonical literal before vendor JSON destruction");
   return 0;
}
