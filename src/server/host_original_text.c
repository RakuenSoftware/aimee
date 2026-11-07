#include "host_original_text.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int installed, sealed, required_mode;
static host_text_resolve_fn resolver;
static host_text_transform_fn transformer;
static void *state;
static void clear(void *p, size_t n)
{
   volatile unsigned char *v = p;
   while (n--)
      *v++ = 0;
}
static int uuid(const char *s)
{
   if (strlen(s) != 36)
      return 0;
   int nonzero = 0;
   for (int i = 0; i < 36; i++)
   {
      if (i == 8 || i == 13 || i == 18 || i == 23)
      {
         if (s[i] != '-')
            return 0;
      }
      else
      {
         if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return 0;
         nonzero |= s[i] != '0';
      }
   }
   return nonzero;
}
static int bounded(const char *s, size_t n)
{
   return memchr(s, 0, n) && s[0];
}
static int utf8(const unsigned char *s, size_t n)
{
   size_t i = 0;
   while (i < n)
   {
      unsigned a = s[i++];
      if (!a)
         return 0;
      if (a < 128)
         continue;
      unsigned cp;
      int k;
      if (a >= 0xc2 && a <= 0xdf)
      {
         cp = a & 31;
         k = 1;
      }
      else if (a >= 0xe0 && a <= 0xef)
      {
         cp = a & 15;
         k = 2;
      }
      else if (a >= 0xf0 && a <= 0xf4)
      {
         cp = a & 7;
         k = 3;
      }
      else
         return 0;
      if (i + (size_t)k > n)
         return 0;
      int total = k;
      while (k--)
      {
         unsigned b = s[i++];
         if ((b & 0xc0) != 0x80)
            return 0;
         cp = (cp << 6) | (b & 63);
      }
      if ((total == 1 && cp < 128) || (total == 2 && cp < 2048) || (total == 3 && cp < 65536) ||
          cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
         return 0;
   }
   return 1;
}
static host_text_status failure(host_text_status s)
{
   switch (s)
   {
   case HOST_TEXT_UNAVAILABLE:
   case HOST_TEXT_UNSUPPORTED:
   case HOST_TEXT_REFUSED:
   case HOST_TEXT_UNKNOWN:
      return s;
   default:
      return HOST_TEXT_INVALID;
   }
}
__attribute__((used, externally_visible, retain)) int
host_text_install(int required, host_text_resolve_fn r, host_text_transform_fn t, void *ctx)
{
   pthread_mutex_lock(&lock);
   int ok = !installed && !sealed && (required == 0 || required == 1) && ((!r && !t) || (r && t));
   if (ok)
   {
      installed = 1;
      required_mode = required;
      resolver = r;
      transformer = t;
      state = ctx;
   }
   pthread_mutex_unlock(&lock);
   return ok ? 0 : -1;
}
__attribute__((used, externally_visible, retain)) int host_text_required(void)
{
   pthread_mutex_lock(&lock);
   int r = required_mode;
   pthread_mutex_unlock(&lock);
   return r;
}
__attribute__((used, externally_visible, retain)) void host_text_destroy(host_text_owned *o)
{
   if (!o)
      return;
   if (o->text)
   {
      clear(o->text, HOST_TEXT_MAX_OUTPUT + 1);
      free(o->text);
   }
   if (o->sidecars_json)
   {
      clear(o->sidecars_json, HOST_TEXT_MAX_SIDECARS + 1);
      free(o->sidecars_json);
   }
   memset(o, 0, sizeof *o);
}
__attribute__((used, externally_visible, retain)) host_text_status
host_text_apply(host_text_kind kind, const char *text, size_t n, host_text_owned *out)
{
   if (!out)
      return HOST_TEXT_INVALID;
   memset(out, 0, sizeof *out);
   pthread_mutex_lock(&lock);
   sealed = 1;
   int required = required_mode;
   host_text_resolve_fn r = resolver;
   host_text_transform_fn t = transformer;
   void *ctx = state;
   pthread_mutex_unlock(&lock);
   if (!r || !t)
      return required ? HOST_TEXT_UNAVAILABLE : HOST_TEXT_PASSTHROUGH;
   if ((kind != HOST_TEXT_PLAIN_PROMPT && kind != HOST_TEXT_EXTERNAL_TRANSCRIPT) || !text ||
       n > HOST_TEXT_MAX_INPUT || !utf8((const unsigned char *)text, n))
      return HOST_TEXT_INVALID;
   host_text_origin origin = {0};
   host_text_status s = r(kind, &origin, ctx);
   if (s != HOST_TEXT_OK)
      return failure(s);
   if (origin.schema_version != 1 || origin.kind != kind ||
       !memchr(origin.operation_uuid, 0, sizeof origin.operation_uuid) ||
       !uuid(origin.operation_uuid) || !bounded(origin.principal, sizeof origin.principal) ||
       !bounded(origin.session_id, sizeof origin.session_id) ||
       !bounded(origin.transport_identity, sizeof origin.transport_identity))
      return HOST_TEXT_INVALID;
   /* No attested original-segment provenance or checkpoint contract is installed. */
   if (kind == HOST_TEXT_EXTERNAL_TRANSCRIPT)
      return HOST_TEXT_UNSUPPORTED;
   out->text = calloc(HOST_TEXT_MAX_OUTPUT + 1, 1);
   out->sidecars_json = calloc(HOST_TEXT_MAX_SIDECARS + 1, 1);
   if (!out->text || !out->sidecars_json)
   {
      host_text_destroy(out);
      return HOST_TEXT_UNAVAILABLE;
   }
   s = t(&origin, text, n, out->text, HOST_TEXT_MAX_OUTPUT + 1, &out->text_len, out->sidecars_json,
         HOST_TEXT_MAX_SIDECARS + 1, &out->sidecars_len, ctx);
   if (s != HOST_TEXT_OK)
   {
      host_text_destroy(out);
      return failure(s);
   }
   if (out->text_len > HOST_TEXT_MAX_OUTPUT || out->sidecars_len > HOST_TEXT_MAX_SIDECARS ||
       out->text[out->text_len] || out->sidecars_json[out->sidecars_len] ||
       !utf8((unsigned char *)out->text, out->text_len) ||
       !utf8((unsigned char *)out->sidecars_json, out->sidecars_len))
   {
      host_text_destroy(out);
      return HOST_TEXT_INVALID;
   }
   out->transformed = 1;
   return HOST_TEXT_OK;
}

__attribute__((used, externally_visible, retain))
const unsigned aimee_original_text_hook_contract_v1 = 1;
