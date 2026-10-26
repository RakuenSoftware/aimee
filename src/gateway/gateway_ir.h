/* Canonical gateway admission and read-only views. No transport or model DTO. */
#pragma once
#include "gateway_platform.h"
#include <aimee/ir/aimee_ir.h>
#include "cJSON.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline aimee_context_meta_t gateway_context(aimee_context_origin_t origin,
                                                   aimee_context_authority_t authority, int visible)
{
   return (aimee_context_meta_t){.origin = origin,
                                 .authority = authority,
                                 .trust = AIMEE_CTX_TRUST_UNVERIFIED,
                                 .sensitivity = AIMEE_CTX_SENS_INTERNAL,
                                 .model_visible = visible};
}
static inline int gateway_message_admit(const aimee_message_t *message)
{
   aimee_request_t view = {.messages = (aimee_message_t *)message, .n_messages = 1};
   return message ? aimee_ir_request_admit(&view) : -1;
}
static inline int gateway_message_clone(const aimee_message_t *source, aimee_message_t *out)
{
   aimee_request_t view = {.messages = (aimee_message_t *)source, .n_messages = 1}, copy = {0};
   if (!out || gateway_message_admit(source) || aimee_ir_request_clone(&view, &copy))
      return -1;
   *out = copy.messages[0];
   free(copy.messages);
   return 0;
}
static inline int gateway_audio_block(const aimee_block_t *block)
{
   return block && block->type == AIMEE_BLK_DOCUMENT && block->media_type &&
          !strncmp(block->media_type, "audio/", 6);
}
static inline const char *gateway_asset_path(const aimee_block_t *block)
{
   return block && block->media_ref && !strncmp(block->media_ref, "file://", 7)
              ? block->media_ref + 7
              : NULL;
}
static inline int gateway_file_hash(const char *path, char out[65])
{
   FILE *file = fopen(path, "rb");
   EVP_MD_CTX *hash = EVP_MD_CTX_new();
   unsigned char buffer[8192], digest[EVP_MAX_MD_SIZE];
   unsigned length = 0;
   int ok = file && hash && EVP_DigestInit_ex(hash, EVP_sha256(), NULL);
   size_t n;
   while (ok && (n = fread(buffer, 1, sizeof buffer, file)))
      ok = EVP_DigestUpdate(hash, buffer, n);
   ok = ok && !ferror(file) && EVP_DigestFinal_ex(hash, digest, &length) && length == 32;
   if (file)
      fclose(file);
   EVP_MD_CTX_free(hash);
   if (!ok)
      return -1;
   for (unsigned i = 0; i < length; i++)
      snprintf(out + i * 2, 3, "%02x", digest[i]);
   return 0;
}
/* Explicit external ingress only. User text and each source asset stay separate.
 * Generic DOCUMENT is the canonical file asset, including audio MIME types. */
static inline int gateway_message_import_external(const char *text, const attachment_t *attachments,
                                                  int count, aimee_message_t *out)
{
   if (!out || count < 0 || count > AIMEE_IR_MAX_BLOCKS - 1 || (count && !attachments))
      return -1;
   memset(out, 0, sizeof *out);
   out->role = strdup("user");
   out->blocks = calloc((size_t)count + 1, sizeof *out->blocks);
   if (!out->role || !out->blocks)
      goto failed;
   if (text && text[0])
   {
      aimee_block_t *b = out->blocks + out->n_blocks++;
      b->type = AIMEE_BLK_TEXT;
      b->context = gateway_context(AIMEE_CTX_ORIGIN_USER, AIMEE_CTX_AUTH_TASK_INSTRUCTION, 1);
      if (!(b->text = strdup(text)))
         goto failed;
   }
   for (int i = 0; i < count; i++)
   {
      const attachment_t *a = attachments + i;
      if (!a->path[0])
         goto failed;
      const char *mime = a->mime[0] ? a->mime : "application/octet-stream";
      const char *ext = strrchr(a->path, '.');
      if (!strncmp(mime, "application/octet-stream", 24) && ext)
      {
         if (!strcmp(ext, ".ogg") || !strcmp(ext, ".oga") || !strcmp(ext, ".opus"))
            mime = "audio/ogg";
         else if (!strcmp(ext, ".mp3"))
            mime = "audio/mpeg";
         else if (!strcmp(ext, ".wav"))
            mime = "audio/wav";
         else if (!strcmp(ext, ".flac"))
            mime = "audio/flac";
         else if (!strcmp(ext, ".m4a"))
            mime = "audio/mp4";
      }
      aimee_block_t *b = out->blocks + out->n_blocks++;
      b->type = !strncmp(mime, "image/", 6) ? AIMEE_BLK_IMAGE : AIMEE_BLK_DOCUMENT;
      b->context = gateway_context(AIMEE_CTX_ORIGIN_USER, AIMEE_CTX_AUTH_EVIDENCE, 0);
      size_t length = strlen(a->path) + 8;
      b->media_ref = malloc(length);
      b->media_type = strdup(mime);
      b->raw = cJSON_CreateObject();
      char digest[65];
      if (!b->media_ref || !b->media_type || !b->raw)
         goto failed;
      int hash_available = !gateway_file_hash(a->path, digest);
      snprintf(b->media_ref, length, "file://%s", a->path);
      cJSON *source = cJSON_AddObjectToObject(b->raw, "gateway_source");
      if (!source || !cJSON_AddBoolToObject(source, "hash_available", hash_available) ||
          (hash_available && !cJSON_AddStringToObject(source, "sha256", digest)) ||
          !cJSON_AddStringToObject(source, "mime", a->mime))
         goto failed;
   }
   if (gateway_message_admit(out))
      goto failed;
   return 0;
failed:
   aimee_message_free(out);
   return -1;
}
/* Read-only view for slash parsing or an explicit external rendering edge. */
static inline char *gateway_message_text_view(const aimee_message_t *messages, int count)
{
   size_t size = 1;
   for (int m = 0; m < count; m++)
      for (int i = 0; i < messages[m].n_blocks; i++)
      {
         const aimee_block_t *b = messages[m].blocks + i;
         if (b->type == AIMEE_BLK_TEXT && aimee_ir_block_model_visible(b) && b->text)
         {
            size_t n = strlen(b->text);
            if (n > SIZE_MAX - size)
               return NULL;
            size += n;
         }
      }
   char *text = malloc(size);
   if (!text)
      return NULL;
   text[0] = 0;
   size_t used = 0;
   for (int m = 0; m < count; m++)
      for (int i = 0; i < messages[m].n_blocks; i++)
      {
         const aimee_block_t *b = messages[m].blocks + i;
         if (b->type == AIMEE_BLK_TEXT && aimee_ir_block_model_visible(b) && b->text)
         {
            size_t n = strlen(b->text);
            memcpy(text + used, b->text, n);
            used += n;
         }
      }
   text[used] = 0;
   return text;
}

static inline int gateway_message_append(aimee_message_t *message, aimee_block_t *owned)
{
   if (message->n_blocks >= AIMEE_IR_MAX_BLOCKS)
      return -1;
   aimee_block_t *blocks =
       realloc(message->blocks, (size_t)(message->n_blocks + 1) * sizeof *blocks);
   if (!blocks)
      return -1;
   message->blocks = blocks;
   blocks[message->n_blocks++] = *owned;
   memset(owned, 0, sizeof *owned);
   message->content_scalar = 0;
   return 0;
}

static inline int gateway_message_append_transcript(aimee_message_t *message, int source_index,
                                                    const char *transcript)
{
   if (!message || source_index < 0 || source_index >= message->n_blocks || !transcript ||
       !transcript[0])
      return -1;
   const aimee_block_t *asset = message->blocks + source_index;
   const char *path = gateway_asset_path(asset);
   const cJSON *source = cJSON_GetObjectItemCaseSensitive(asset->raw, "gateway_source");
   const char *expected = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(source, "sha256"));
   char digest[65];
   if (!gateway_audio_block(asset) || !path || !expected || gateway_file_hash(path, digest) ||
       strcmp(expected, digest))
      return -1;
   aimee_block_t derived = {
       .type = AIMEE_BLK_TEXT,
       .context = gateway_context(AIMEE_CTX_ORIGIN_USER, AIMEE_CTX_AUTH_TASK_INSTRUCTION, 1)};
   derived.text = strdup(transcript);
   derived.raw = cJSON_CreateObject();
   cJSON *provenance =
       derived.raw ? cJSON_AddObjectToObject(derived.raw, "gateway_transcript") : NULL;
   if (!derived.text || !provenance ||
       !cJSON_AddStringToObject(provenance, "source_sha256", digest) ||
       !cJSON_AddStringToObject(provenance, "source_media_ref", asset->media_ref) ||
       gateway_message_append(message, &derived))
   {
      aimee_block_free_contents(&derived);
      return -1;
   }
   return 0;
}
