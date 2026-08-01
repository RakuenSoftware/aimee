/* platform_ntfy.c: ntfy.sh outbound push adapter. */
#include "platform_ntfy.h"
#include "gateway_platform.h"
#include "gateway_ctx.h"
#include "aimee.h"
#include "agent_exec.h"
#include "delivery_target.h"
#include "log.h"
#include "runtime_secret.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_BASE_URL "https://ntfy.sh"

static char ntfy_base_url[256] = DEFAULT_BASE_URL;
static char ntfy_token[256] = {0};

static int ntfy_check_config(platform_adapter_t *self, char *err_out, size_t err_len)
{
   (void)self;
   (void)err_out;
   (void)err_len;
   /* ntfy has a usable default base URL, so check always passes. */
   return 0;
}

static int ntfy_startup(platform_adapter_t *self, gateway_ctx_t *ctx)
{
   (void)self;
   (void)ctx;
   /* No polling thread needed; outbound only. */
   return 0;
}

static void ntfy_shutdown(platform_adapter_t *self)
{
   (void)self;
   runtime_secret_wipe(ntfy_token, sizeof(ntfy_token));
}

static int ntfy_send_text(platform_adapter_t *self, const delivery_target_t *target,
                          const char *text)
{
   char url[512];
   char *auth_header = NULL;

   (void)self;

   if (target == NULL || target->chat_id[0] == '\0')
   {
      aimee_log(LOG_ERROR, "ntfy", "send_text: NULL target or empty chat_id");
      return -1;
   }

   if (strlen(ntfy_base_url) + strlen(target->chat_id) + 2 > sizeof(url))
   {
      aimee_log(LOG_ERROR, "ntfy", "send_text: URL too long");
      return -1;
   }
   snprintf(url, sizeof(url), "%s/%s", ntfy_base_url, target->chat_id);

   if (ntfy_token[0] != '\0')
   {
      size_t needed = strlen("Authorization: Bearer ") + strlen(ntfy_token) + 1;
      auth_header = malloc(needed);
      if (auth_header == NULL)
      {
         aimee_log(LOG_ERROR, "ntfy", "send_text: OOM");
         return -1;
      }
      snprintf(auth_header, needed, "Authorization: Bearer %s", ntfy_token);
   }

   int rc = agent_http_post_content_type(url, auth_header, "text/plain", text, NULL, 30000, NULL);

   if (auth_header != NULL)
   {
      free(auth_header);
   }

   if (rc != 0)
   {
      aimee_log(LOG_ERROR, "ntfy", "send_text: HTTP POST failed (%d)", rc);
      return -1;
   }

   return 0;
}

static int ntfy_send_attachment(platform_adapter_t *self, const delivery_target_t *target,
                                const char *path, const char *mime)
{
   char url[512];
   char *auth_header = NULL;
   char *file_content = NULL;
   long file_size = 0;
   int rc = -1;
   FILE *f;

   (void)self;

   if (target == NULL || target->chat_id[0] == '\0')
   {
      aimee_log(LOG_ERROR, "ntfy", "send_attachment: NULL target or empty chat_id");
      return -1;
   }

   if (path == NULL)
   {
      aimee_log(LOG_ERROR, "ntfy", "send_attachment: NULL path");
      return -1;
   }

   f = fopen(path, "rb");
   if (f == NULL)
   {
      aimee_log(LOG_ERROR, "ntfy", "send_attachment: cannot open %s", path);
      return -1;
   }

   fseek(f, 0, SEEK_END);
   file_size = ftell(f);
   fseek(f, 0, SEEK_SET);

   if (file_size <= 0 || file_size > 10 * 1024 * 1024)
   {
      aimee_log(LOG_ERROR, "ntfy", "send_attachment: bad file size %ld", file_size);
      fclose(f);
      return -1;
   }

   file_content = malloc((size_t)file_size + 1);
   if (file_content == NULL)
   {
      fclose(f);
      aimee_log(LOG_ERROR, "ntfy", "send_attachment: OOM");
      return -1;
   }

   if (fread(file_content, 1, (size_t)file_size, f) != (size_t)file_size)
   {
      aimee_log(LOG_ERROR, "ntfy", "send_attachment: read error");
      free(file_content);
      fclose(f);
      return -1;
   }
   file_content[file_size] = '\0';
   fclose(f);

   if (strlen(ntfy_base_url) + strlen(target->chat_id) + 2 > sizeof(url))
   {
      free(file_content);
      aimee_log(LOG_ERROR, "ntfy", "send_attachment: URL too long");
      return -1;
   }
   snprintf(url, sizeof(url), "%s/%s", ntfy_base_url, target->chat_id);

   if (ntfy_token[0] != '\0')
   {
      size_t needed = strlen("Authorization: Bearer ") + strlen(ntfy_token) + 1;
      auth_header = malloc(needed);
      if (auth_header == NULL)
      {
         free(file_content);
         aimee_log(LOG_ERROR, "ntfy", "send_attachment: OOM");
         return -1;
      }
      snprintf(auth_header, needed, "Authorization: Bearer %s", ntfy_token);
   }

   rc = agent_http_post_content_type(url, auth_header, mime ? mime : "application/octet-stream",
                                     file_content, NULL, 60000, NULL);

   if (auth_header != NULL)
      free(auth_header);
   free(file_content);

   if (rc != 0)
   {
      aimee_log(LOG_ERROR, "ntfy", "send_attachment: HTTP POST failed (%d)", rc);
      return -1;
   }

   return 0;
}

static int ntfy_authorize_user(platform_adapter_t *self, const char *platform, const char *chat_id,
                               const char *user_id)
{
   (void)self;
   (void)platform;
   (void)chat_id;
   (void)user_id;
   /* ntfy is outbound-only; no inbound auth needed. */
   return 0;
}

static int ntfy_set_typing(platform_adapter_t *self, const delivery_target_t *target, int typing)
{
   (void)self;
   (void)target;
   (void)typing;
   /* ntfy has no typing indicator support. */
   return 0;
}

static platform_adapter_t ntfy_adapter = {
    .name = "ntfy",
    .display_name = "ntfy",
    .enabled = 0,
    .startup = ntfy_startup,
    .shutdown = ntfy_shutdown,
    .check_config = ntfy_check_config,
    .send_text = ntfy_send_text,
    .send_attachment = ntfy_send_attachment,
    .authorize_user = ntfy_authorize_user,
    .set_typing = ntfy_set_typing,
    .user = NULL,
};

static int ntfy_init(void)
{
   const char *env_url = getenv("AIMEE_GATEWAY_NTFY_BASE_URL");
   char vault_token[sizeof(ntfy_token)] = "";

   if (env_url != NULL && env_url[0] != '\0')
   {
      strncpy(ntfy_base_url, env_url, sizeof(ntfy_base_url) - 1);
      ntfy_base_url[sizeof(ntfy_base_url) - 1] = '\0';
   }

   if (runtime_secret_get("AIMEE_GATEWAY_NTFY_TOKEN", vault_token, sizeof(vault_token)))
   {
      strncpy(ntfy_token, vault_token, sizeof(ntfy_token) - 1);
      ntfy_token[sizeof(ntfy_token) - 1] = '\0';
   }
   runtime_secret_wipe(vault_token, sizeof(vault_token));

   return gateway_platform_register(&ntfy_adapter);
}

platform_adapter_t *ntfy_adapter_get(void)
{
   ntfy_init();
   return &ntfy_adapter;
}
