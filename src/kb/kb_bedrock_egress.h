#ifndef KB_BEDROCK_EGRESS_H
#define KB_BEDROCK_EGRESS_H
#include <stddef.h>
#include <aimee/translation/aimee_backend.h>
#include "../modules/aws/aws_sigv4.h"
#include "../modules/aws/aws_eventstream.h"
typedef struct
{
   const char *model_id, *region, *partition, *endpoint;
} kb_bedrock_target_t;
typedef struct
{
   char path[1024], body[65536], host[512];
   aws_sigv4_result_t sig;
} kb_bedrock_request_t;
int kb_bedrock_build_request(const kb_bedrock_target_t *, const aimee_request_t *, int,
                             const char *, const char *, const char *, const char *, const char *,
                             kb_bedrock_request_t *);
typedef int (*kb_bedrock_delta_cb)(const char *, const char *, size_t, void *);
int kb_bedrock_decode_stream(const unsigned char *, size_t, kb_bedrock_delta_cb, void *, int *);
int kb_bedrock_dispatch_https(const kb_bedrock_target_t *, const aimee_request_t *, int,
                              const char *, const char *, const char *, const char *, const char *,
                              const char *, const char *, char *, size_t, int *);
#endif
