/* platform_telegram.c: Telegram Bot API long-polling platform adapter. */
#include "platform_telegram.h"

#include "gateway_ctx.h"
#include "aimee.h"
#include "agent_exec.h"
#include "log.h"
#include "delivery_target.h"
#include "runtime_secret.h"

#include <cJSON.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stddef.h>

#define TELEGRAM_API_HOST                 "https://api.telegram.org"
#define TELEGRAM_POLL_TIMEOUT             30
#define TELEGRAM_POLL_ERROR_PAUSE_SECONDS 5
#define TELEGRAM_MAX_PARSE_ERRORS         10

#define ENV_TOKEN         "AIMEE_GATEWAY_TELEGRAM_TOKEN"
#define ENV_ALLOWED_USERS "AIMEE_GATEWAY_TELEGRAM_ALLOWED_USERS"

typedef struct telegram_ctx
{
   gateway_ctx_t *gw;
   pthread_t poller;
   volatile int stop;
   int64_t offset;
   int parse_errors;
   char token[256];
   char allowed_users[1024];
} telegram_ctx_t;

static int tg_http_post(telegram_ctx_t *ctx, const char *method, const char *body, char **out)
{
   char url[512];
   snprintf(url, sizeof(url), "%s/bot%s/%s", TELEGRAM_API_HOST, ctx->token, method);
   return agent_http_post(url, NULL, body, out, 30000, NULL);
}

static int tg_build_get_updates_url(telegram_ctx_t *ctx, char *buf, size_t bufsz)
{
   return snprintf(buf, bufsz, "%s/bot%s/getUpdates?offset=%lld&timeout=%d", TELEGRAM_API_HOST,
                   ctx->token, (long long)ctx->offset, TELEGRAM_POLL_TIMEOUT);
}

static int tg_check_config(platform_adapter_t *self, char *err_out, size_t err_len)
{
   telegram_ctx_t *ctx = (telegram_ctx_t *)self->user;
   if (!ctx)
   {
      if (err_out && err_len > 0)
         snprintf(err_out, err_len, "telegram adapter: no context");
      return -1;
   }

   char token[sizeof(ctx->token)] = "";
   if (!runtime_secret_get(ENV_TOKEN, token, sizeof(token)))
   {
      if (err_out && err_len > 0)
         snprintf(err_out, err_len, "AIMEE_GATEWAY_TELEGRAM_TOKEN is not set");
      return -1;
   }

   const char *allowed = getenv(ENV_ALLOWED_USERS);
   if (!allowed)
   {
      if (err_out && err_len > 0)
         snprintf(err_out, err_len, "AIMEE_GATEWAY_TELEGRAM_ALLOWED_USERS is not set");
      runtime_secret_wipe(token, sizeof(token));
      return -1;
   }

   snprintf(ctx->token, sizeof(ctx->token), "%s", token);
   runtime_secret_wipe(token, sizeof(token));
   snprintf(ctx->allowed_users, sizeof(ctx->allowed_users), "%s", allowed);
   return 0;
}

static int process_file_message(telegram_ctx_t *ctx, cJSON *msg)
{
   cJSON *file_id_json = cJSON_GetObjectItem(msg, "voice");
   if (!file_id_json)
      file_id_json = cJSON_GetObjectItem(msg, "document");
   if (!file_id_json)
      return 0;

   /* Guard file_id: a voice/document object without a string file_id must not
    * dereference NULL (untrusted Telegram payload). */
   const char *file_id = NULL;
   cJSON *fid = cJSON_GetObjectItem(file_id_json, "file_id");
   if (cJSON_IsString(fid))
      file_id = fid->valuestring;

   /* chat.id is a JSON number in the Telegram API — the downstream src.chat_id
    * is built from cid->valueint, not valuestring. Reading it as a string here
    * always yielded NULL, so this guard previously dropped every file message.
    * Require chat.id as a numeric presence guard instead. */
   int have_chat_id = 0;
   cJSON *chat = cJSON_GetObjectItem(msg, "chat");
   if (chat)
   {
      cJSON *cid = cJSON_GetObjectItem(chat, "id");
      if (cJSON_IsNumber(cid))
         have_chat_id = 1;
   }

   if (!file_id || !have_chat_id)
      return 0;

   /* Get file path via getFile */
   char getfile_body[512];
   snprintf(getfile_body, sizeof(getfile_body), "{\"file_id\":\"%s\"}", file_id);
   char *getfile_resp = NULL;
   int rc = tg_http_post(ctx, "getFile", getfile_body, &getfile_resp);
   (void)rc;
   if (rc != 0 || !getfile_resp)
      return 0;

   cJSON *getfile_json = cJSON_Parse(getfile_resp);
   free(getfile_resp);
   if (!getfile_json)
      return 0;

   cJSON *result = cJSON_GetObjectItem(getfile_json, "result");
   const char *file_path = NULL;
   if (result)
   {
      cJSON *fp = cJSON_GetObjectItem(result, "file_path");
      if (fp)
         file_path = fp->valuestring;
   }
   if (!file_path)
   {
      cJSON_Delete(getfile_json);
      return 0;
   }

   /* Download file */
   cJSON *update = cJSON_GetObjectItem(msg, "__update");
   int64_t update_id = 0;
   if (update)
   {
      cJSON *uid = cJSON_GetObjectItem(update, "update_id");
      if (uid)
         update_id = uid->valueint;
   }

   char download_url[512];
   snprintf(download_url, sizeof(download_url), "https://api.telegram.org/file/bot%s/%s",
            ctx->token, file_path);

   char *download_resp = NULL;
   int dl_rc = agent_http_get(download_url, NULL, &download_resp, 60000);
   if (dl_rc != 0 || !download_resp)
   {
      cJSON_Delete(getfile_json);
      return 0;
   }

   char local_path[512];
   const char *ext = strstr(file_path, ".oga") ? ".oga" : ".bin";
   snprintf(local_path, sizeof(local_path), "/tmp/aimee-tg-voice-%lld%s", (long long)update_id,
            ext);

   FILE *fp = fopen(local_path, "wb");
   if (fp)
   {
      fwrite(download_resp, 1, strlen(download_resp), fp);
      fclose(fp);
   }
   free(download_resp);

   /* Build source and attachments */
   session_source_t src;
   memset(&src, 0, sizeof(src));
   snprintf(src.platform, sizeof(src.platform), "telegram");

   if (chat)
   {
      cJSON *ct = cJSON_GetObjectItem(chat, "type");
      if (ct && ct->valuestring)
         snprintf(src.chat_type, sizeof(src.chat_type), "%s", ct->valuestring);
      cJSON *cid = cJSON_GetObjectItem(chat, "id");
      if (cid)
         snprintf(src.chat_id, sizeof(src.chat_id), "%lld", (long long)cid->valueint);
   }

   cJSON *from = cJSON_GetObjectItem(msg, "from");
   if (from)
   {
      cJSON *uid = cJSON_GetObjectItem(from, "id");
      if (uid)
         snprintf(src.user_id, sizeof(src.user_id), "%lld", (long long)uid->valueint);
   }

   cJSON *thread_id_json = cJSON_GetObjectItem(msg, "message_thread_id");
   if (thread_id_json && thread_id_json->valueint != 0)
      snprintf(src.thread_id, sizeof(src.thread_id), "%lld", (long long)thread_id_json->valueint);

   attachment_t attach;
   memset(&attach, 0, sizeof(attach));
   snprintf(attach.path, sizeof(attach.path), "%s", local_path);
   snprintf(attach.mime, sizeof(attach.mime), "application/octet-stream");

   gateway_handle_message(ctx->gw, &src, "", &attach, 1);
   cJSON_Delete(getfile_json);
   return 1;
}

static void *polling_thread(void *arg)
{
   telegram_ctx_t *ctx = (telegram_ctx_t *)arg;
   while (!ctx->stop)
   {
      char url[512];
      tg_build_get_updates_url(ctx, url, sizeof(url));
      char *resp = NULL;
      int rc = agent_http_get(url, NULL, &resp, (TELEGRAM_POLL_TIMEOUT + 5) * 1000);
      if (rc != 0 || !resp)
      {
         LOG_WARN("telegram", "polling failed, pausing %ds", TELEGRAM_POLL_ERROR_PAUSE_SECONDS);
         sleep(TELEGRAM_POLL_ERROR_PAUSE_SECONDS);
         continue;
      }

      cJSON *json = cJSON_Parse(resp);
      free(resp);
      if (!json)
      {
         LOG_WARN("telegram", "JSON parse error (consecutive %d)", ctx->parse_errors + 1);
         ctx->parse_errors++;
         if (ctx->parse_errors >= TELEGRAM_MAX_PARSE_ERRORS)
         {
            LOG_ERROR("telegram", "too many parse errors, stopping");
            break;
         }
         sleep(TELEGRAM_POLL_ERROR_PAUSE_SECONDS);
         continue;
      }
      ctx->parse_errors = 0;

      cJSON *ok_json = cJSON_GetObjectItem(json, "ok");
      if (!ok_json || !ok_json->valueint)
      {
         LOG_WARN("telegram", "getUpdates returned ok=false");
         cJSON_Delete(json);
         sleep(TELEGRAM_POLL_ERROR_PAUSE_SECONDS);
         continue;
      }

      cJSON *results = cJSON_GetObjectItem(json, "result");
      if (!cJSON_IsArray(results))
      {
         cJSON_Delete(json);
         sleep(TELEGRAM_POLL_ERROR_PAUSE_SECONDS);
         continue;
      }

      cJSON *item;
      cJSON_ArrayForEach(item, results)
      {
         cJSON *update_id_json = cJSON_GetObjectItem(item, "update_id");
         if (update_id_json)
            ctx->offset = (int64_t)update_id_json->valueint + 1;

         cJSON *msg = cJSON_GetObjectItem(item, "message");
         if (!msg)
            msg = cJSON_GetObjectItem(item, "edited_message");
         if (!msg)
            msg = cJSON_GetObjectItem(item, "channel_post");
         if (!msg)
            continue;

         /* Attach the update object for file processing */
         cJSON_AddItemToObject(msg, "__update", cJSON_DetachItemFromObject(item, "update_id"));
         if (process_file_message(ctx, msg))
            continue;

         cJSON *chat = cJSON_GetObjectItem(msg, "chat");
         cJSON *from = cJSON_GetObjectItem(msg, "from");

         session_source_t src;
         memset(&src, 0, sizeof(src));
         snprintf(src.platform, sizeof(src.platform), "telegram");

         if (chat)
         {
            cJSON *ct = cJSON_GetObjectItem(chat, "type");
            if (ct && ct->valuestring)
               snprintf(src.chat_type, sizeof(src.chat_type), "%s", ct->valuestring);
            cJSON *cid = cJSON_GetObjectItem(chat, "id");
            if (cid)
               snprintf(src.chat_id, sizeof(src.chat_id), "%lld", (long long)cid->valueint);
         }

         if (from)
         {
            cJSON *uid = cJSON_GetObjectItem(from, "id");
            if (uid)
               snprintf(src.user_id, sizeof(src.user_id), "%lld", (long long)uid->valueint);
         }

         cJSON *thread_id_json = cJSON_GetObjectItem(msg, "message_thread_id");
         if (thread_id_json && thread_id_json->valueint != 0)
            snprintf(src.thread_id, sizeof(src.thread_id), "%lld",
                     (long long)thread_id_json->valueint);

         cJSON *text_json = cJSON_GetObjectItem(msg, "text");
         const char *text = text_json ? text_json->valuestring : "";

         gateway_handle_message(ctx->gw, &src, text, NULL, 0);
      }

      cJSON_Delete(json);
   }
   return NULL;
}

static int tg_startup(platform_adapter_t *self, gateway_ctx_t *gw)
{
   telegram_ctx_t *ctx = (telegram_ctx_t *)self->user;
   ctx->gw = gw;
   ctx->stop = 0;
   ctx->offset = 0;
   ctx->parse_errors = 0;

   if (pthread_create(&ctx->poller, NULL, polling_thread, ctx) != 0)
   {
      LOG_ERROR("telegram", "failed to create polling thread");
      return -1;
   }

   self->enabled = 1;
   LOG_INFO("telegram", "polling thread started");
   return 0;
}

static void tg_shutdown(platform_adapter_t *self)
{
   telegram_ctx_t *ctx = (telegram_ctx_t *)self->user;
   ctx->stop = 1;
   pthread_join(ctx->poller, NULL);
   runtime_secret_wipe(ctx->token, sizeof(ctx->token));
   self->enabled = 0;
}

static int tg_send_text(platform_adapter_t *self, const delivery_target_t *target, const char *text)
{
   telegram_ctx_t *ctx = (telegram_ctx_t *)self->user;
   cJSON *body = cJSON_CreateObject();
   if (!body)
      return -1;
   cJSON_AddStringToObject(body, "chat_id", target->chat_id);
   cJSON_AddStringToObject(body, "text", text ? text : "");

   char *body_str = cJSON_PrintUnformatted(body);
   char *resp = NULL;
   int rc = tg_http_post(ctx, "sendMessage", body_str, &resp);

   free(body_str);
   cJSON_Delete(body);
   if (resp)
      free(resp);

   return rc;
}

static int tg_send_attachment(platform_adapter_t *self, const delivery_target_t *target,
                              const char *path, const char *mime)
{
   (void)self;
   (void)target;
   (void)path;
   (void)mime;
   /* Multipart upload would be needed; placeholders only */
   return -1;
}

static int tg_authorize_user(platform_adapter_t *self, const char *platform, const char *chat_id,
                             const char *user_id)
{
   (void)self;
   (void)chat_id;

   if (!platform || strcmp(platform, "telegram") != 0)
      return -1;

   const char *allowed = getenv(ENV_ALLOWED_USERS);
   if (!allowed || strlen(allowed) == 0)
      return -1;

   if (!user_id || strlen(user_id) == 0)
      return -1;

   /* Parse comma-separated allowlist */
   char allowlist_copy[1024];
   snprintf(allowlist_copy, sizeof(allowlist_copy), "%s", allowed);

   char *saveptr = NULL;
   char *tok = strtok_r(allowlist_copy, ",", &saveptr);
   while (tok)
   {
      while (*tok == ' ')
         tok++;
      if (strcmp(tok, user_id) == 0)
         return 0;
      tok = strtok_r(NULL, ",", &saveptr);
   }

   return -1;
}

static int tg_set_typing(platform_adapter_t *self, const delivery_target_t *target, int typing)
{
   telegram_ctx_t *ctx = (telegram_ctx_t *)self->user;
   const char *action = typing ? "typing" : "cancel";

   cJSON *body = cJSON_CreateObject();
   if (!body)
      return -1;
   cJSON_AddStringToObject(body, "chat_id", target->chat_id);
   cJSON_AddStringToObject(body, "action", action);

   char *body_str = cJSON_PrintUnformatted(body);
   char *resp = NULL;
   int rc = tg_http_post(ctx, "sendChatAction", body_str, &resp);

   free(body_str);
   cJSON_Delete(body);
   if (resp)
      free(resp);

   return rc;
}

static platform_adapter_t s_telegram_adapter;

platform_adapter_t *telegram_adapter_get(void)
{
   static telegram_ctx_t s_ctx;
   static int s_initialised = 0;

   if (!s_initialised)
   {
      memset(&s_ctx, 0, sizeof(s_ctx));
      memset(&s_telegram_adapter, 0, sizeof(s_telegram_adapter));
      s_telegram_adapter.name = "telegram";
      s_telegram_adapter.display_name = "Telegram";
      s_telegram_adapter.enabled = 0;
      s_telegram_adapter.startup = tg_startup;
      s_telegram_adapter.shutdown = tg_shutdown;
      s_telegram_adapter.check_config = tg_check_config;
      s_telegram_adapter.send_text = tg_send_text;
      s_telegram_adapter.send_attachment = tg_send_attachment;
      s_telegram_adapter.authorize_user = tg_authorize_user;
      s_telegram_adapter.set_typing = tg_set_typing;
      s_telegram_adapter.user = &s_ctx;
      s_initialised = 1;
   }

   return &s_telegram_adapter;
}
