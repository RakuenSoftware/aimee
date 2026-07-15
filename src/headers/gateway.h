#ifndef DEC_GATEWAY_H
#define DEC_GATEWAY_H 1

#include "delivery_target.h"
#include <stddef.h>

typedef struct gateway_ctx gateway_ctx_t;

struct gateway_ctx
{
   const struct gateway_platform_entry *platforms;
   int platform_count;
};

typedef struct session_source
{
   char platform[32];
   char chat_type[32];
   char chat_id[128];
   char user_id[128];
   char thread_id[64];
} session_source_t;

typedef struct attachment
{
   char path[512];
   char mime[128];
} attachment_t;

typedef struct platform_adapter platform_adapter_t;

struct platform_adapter
{
   const char *name;
   const char *display_name;

   int (*startup)(platform_adapter_t *self, gateway_ctx_t *ctx);
   void (*shutdown)(platform_adapter_t *self);
   int (*check_config)(platform_adapter_t *self, char *err_out, size_t err_len);

   int (*send_text)(platform_adapter_t *self, const delivery_target_t *target, const char *text);
   int (*send_attachment)(platform_adapter_t *self, const delivery_target_t *target,
                          const char *path, const char *mime);
   int (*authorize_user)(platform_adapter_t *self, const session_source_t *src);
   int (*set_typing)(platform_adapter_t *self, const delivery_target_t *target, int typing);

   void *user;
};

typedef struct gateway_platform_entry
{
   const char *name;
   const char *display_name;
   int enabled;
   platform_adapter_t *adapter;
} gateway_platform_entry_t;

const gateway_platform_entry_t *gateway_platform_entries(int *count_out);

int gateway_session_key_build(const session_source_t *source, int group_sessions_per_user,
                              char *buf, size_t bufsz);

int delivery_router_send(gateway_ctx_t *ctx, const delivery_target_t *target, const char *text,
                         const attachment_t *attachments, int attachment_count);

#endif /* DEC_GATEWAY_H */
