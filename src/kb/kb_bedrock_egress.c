#include "kb_bedrock_egress.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cJSON.h"
#include "kb_tls.h"
static int bad(const char *s)
{
   if (!s || !*s)
      return 1;
   for (; *s; s++)
      if ((unsigned char)*s < 0x20 || *s == '?' || *s == '#')
         return 1;
   return 0;
}
int kb_bedrock_build_request(const kb_bedrock_target_t *t, const aimee_request_t *ir, int stream,
                             const char *ak, const char *sk, const char *tok, const char *amz,
                             const char *date, kb_bedrock_request_t *o)
{
   if (!t || !ir || !o || bad(t->model_id) || bad(t->region) || bad(ak) || bad(sk) || !amz || !date)
      return -1;
   memset(o, 0, sizeof(*o));
   cJSON *j = bedrock_converse_build(ir);
   if (!j)
      return -1;
   char *b = cJSON_PrintUnformatted(j);
   cJSON_Delete(j);
   if (!b || strlen(b) >= sizeof(o->body))
   {
      free(b);
      return -1;
   }
   strcpy(o->body, b);
   free(b);
   if (snprintf(o->path, sizeof(o->path), "/model/%s%s", t->model_id,
                stream ? "/converse-stream" : "/converse") < 0)
      return -1;
   if (t->endpoint && *t->endpoint)
   {
      if (strncmp(t->endpoint, "https://", 8))
         return -1;
      snprintf(o->host, sizeof(o->host), "%s", t->endpoint + 8);
      char *x = strchr(o->host, '/');
      if (x)
         *x = 0;
   }
   else
      snprintf(o->host, sizeof(o->host), "bedrock-runtime.%s.amazonaws.com", t->region);
   char hash[65];
   aws_sha256_hex((const unsigned char *)o->body, strlen(o->body), hash);
   aws_sigv4_kv_t h[] = {
       {"content-type", "application/json"}, {"host", o->host}, {"x-amz-date", amz}};
   aws_sigv4_request_t r = {"POST", o->path, NULL,      0,         h,  3,  hash,
                            amz,    date,    t->region, "bedrock", ak, sk, tok};
   return aws_sigv4_sign(&r, &o->sig);
}
int kb_bedrock_decode_stream(const unsigned char *b, size_t l, kb_bedrock_delta_cb cb, void *ctx,
                             int *stop)
{
   size_t o = 0;
   if (stop)
      *stop = 0;
   while (o < l)
   {
      aws_es_message_t m;
      size_t n = 0;
      if (aws_es_decode(b + o, l - o, &m, &n) != AWS_ES_OK || m.msg_type != AWS_ES_MSG_EVENT ||
          !m.event_type || m.event_type->value.len >= 128)
         return -1;
      char ev[128];
      memcpy(ev, m.event_type->value.ptr, m.event_type->value.len);
      ev[m.event_type->value.len] = 0;
      if (!strcmp(ev, "messageStop") && stop)
         *stop = 1;
      if (cb && cb(ev, (const char *)m.payload.ptr, m.payload.len, ctx))
         return -1;
      o += n;
   }
   return stop && *stop ? 0 : -1;
}
int kb_bedrock_dispatch_https(const kb_bedrock_target_t *t, const aimee_request_t *ir, int stream,
                              const char *ak, const char *sk, const char *tok, const char *amz,
                              const char *date, const char *ca, const char *cc, char *out,
                              size_t cap, int *status)
{
   if (!ca || !out || !cap)
      return -1;
   kb_bedrock_request_t q;
   if (kb_bedrock_build_request(t, ir, stream, ak, sk, tok, amz, date, &q) != 0)
      return -1;
   char h[2400];
   int n = snprintf(h, sizeof(h), "Authorization: %s\r\nX-Amz-Date: %s\r\n%s", q.sig.authorization,
                    q.sig.amz_date, q.sig.has_security_token ? "X-Amz-Security-Token: " : "");
   if (q.sig.has_security_token)
      n += snprintf(h + n, sizeof(h) - (size_t)n, "%s\r\n", q.sig.security_token);
   if (n <= 0 || (size_t)n >= sizeof(h))
      return -1;
   int port = 443;
   char *colon = strrchr(q.host, ':');
   if (colon && colon[1])
   {
      char *end = NULL;
      long p = strtol(colon + 1, &end, 10);
      if (!end || *end || p < 1 || p > 65535)
         return -1;
      *colon = 0;
      port = (int)p;
   }
   return kb_tls_client_request_auth(q.host, port, ca, cc, NULL, "POST", q.path, q.body, h, out,
                                     cap, status);
}
