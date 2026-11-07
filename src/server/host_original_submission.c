#include "host_original_submission.h"
#include "aimee_sha256.h"
#include "cJSON.h"
#include <aimee/ir/aimee_ir.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#define HOST_ENTRY __attribute__((used, externally_visible, retain))
struct host_original_submission
{
   host_original_submission_owner ops;
   void *owner, *state;
   atomic_int phase;
   pthread_t worker;
   atomic_int admitted;
   host_original_documentary_hooks documentary;
   void *documentary_state;
   unsigned exports;
   int user_source_only;
   host_original_submission_outcome_fn outcome, delivered;
   int already_delivered;
   struct host_original_submission *next;
};
struct host_original_documentary
{
   host_original_documentary_hooks hooks;
   void *private_document, *state;
   unsigned references;
   struct host_original_documentary *next;
};
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static host_original_submission *submissions;
static host_original_documentary *documents;
enum
{
   PENDING,
   CONSUMING,
   ACTIVE,
   FINISHED
};
static pthread_mutex_t installation_lock = PTHREAD_MUTEX_INITIALIZER;
static int installed, sealed, documentary_installed;
static host_original_documentary_hooks configured_documentary;
static void *configured_documentary_state;
static host_original_submission_owner configured;
static void *configured_state;
static host_original_submission_outcome_fn configured_outcome, configured_delivered;
static _Thread_local host_original_submission **pending_slot;
__attribute__((used, externally_visible, retain))
const unsigned aimee_original_submission_handoff_contract_v1 = 1;
__attribute__((used, externally_visible, retain))
const unsigned aimee_original_submission_documentary_contract_v1 = 1;
static void cleanse(char *p, size_t capacity)
{
   if (!p)
      return;
   volatile unsigned char *v = (volatile unsigned char *)p;
   while (capacity--)
      *v++ = 0;
}
/* Only the newly parsed owned DOM. Parser-returned strings are terminated;
 * private callbacks borrow this tree and must not mutate or retain it. */
static void wipe_owned_dom(cJSON *node)
{
   for (cJSON *item = node; item; item = item->next)
   {
      wipe_owned_dom(item->child);
      if (!(item->type & cJSON_IsReference) && item->valuestring)
         cleanse(item->valuestring, strlen(item->valuestring));
      if (!(item->type & cJSON_StringIsConst) && item->string)
         cleanse(item->string, strlen(item->string));
      item->valuedouble = 0;
      item->valueint = 0;
   }
}
HOST_ENTRY
void host_original_submission_result_destroy(host_original_submission_result *out)
{
   if (!out)
      return;
   if (out->text_capacity <= HOST_TEXT_MAX_OUTPUT + 1)
      cleanse(out->text, out->text_capacity);
   free(out->text);
   cJSON_Delete(out->attachment);
   memset(out, 0, sizeof *out);
}
HOST_ENTRY
int host_original_submission_required(void)
{
   pthread_mutex_lock(&installation_lock);
   int required = installed;
   pthread_mutex_unlock(&installation_lock);
   return required;
}
HOST_ENTRY
int host_original_submission_install(const host_original_submission_owner *ops, void *state)
{
   if (!ops || !ops->capture || !ops->consume || !ops->finish || !ops->destroy)
      return -1;
   pthread_mutex_lock(&installation_lock);
   int ok = !installed && !sealed;
   if (ok)
   {
      configured = *ops;
      configured_state = state;
      installed = 1;
   }
   pthread_mutex_unlock(&installation_lock);
   return ok ? 0 : -1;
}
__attribute__((used, externally_visible, retain))
const unsigned aimee_original_input_outcome_callback_contract_v1 = 1;
HOST_ENTRY
int host_original_submission_install_outcome_provider_v1(
    const host_original_submission_owner *expected,
    host_original_submission_outcome_fn callback, void *state)
{
   if (!expected || !callback)
      return -1;
   pthread_mutex_lock(&installation_lock);
   int ok = installed && !sealed && !configured_outcome && configured_state == state &&
       expected->capture == configured.capture && expected->consume == configured.consume &&
       expected->finish == configured.finish && expected->destroy == configured.destroy;
   if (ok)
      configured_outcome = callback;
   pthread_mutex_unlock(&installation_lock);
   return ok ? 0 : -1;
}
HOST_ENTRY
cJSON *host_original_submission_outcome_json_v1(const host_original_submission *cap)
{
   host_original_submission_outcome out = {0};
   int accepted = -1;
   pthread_mutex_lock(&registry_lock);
   const host_original_submission *found = submissions;
   while (found && found != cap)
      found = found->next;
   if (found && found->user_source_only && found->outcome &&
       (atomic_load(&found->phase) == ACTIVE || atomic_load(&found->phase) == FINISHED) &&
       pthread_equal(found->worker, pthread_self()))
      accepted = found->outcome(found->owner, &out, found->state);
   pthread_mutex_unlock(&registry_lock);
   cJSON *result = NULL;
   if (accepted == 0 && out.json && out.length && out.length <= 4096 &&
       out.capacity == out.length + 1 &&
       memchr(out.json, 0, out.capacity) == out.json + out.length)
   {
      result = aimee_ir_json_parse_frame(out.json, out.length);
      if (!cJSON_IsObject(result))
      {
         cJSON_Delete(result);
         result = NULL;
      }
   }
   if (out.json && out.capacity <= 4097)
      cleanse(out.json, out.capacity);
   free(out.json);
   return result;
}
__attribute__((used, externally_visible, retain))
const unsigned aimee_original_input_delivered_callback_contract_v1 = 1;
HOST_ENTRY
int host_original_submission_install_delivered_provider_v1(
    const host_original_submission_owner *expected,
    host_original_submission_outcome_fn callback, void *state)
{
   if (!expected || !callback)
      return -1;
   pthread_mutex_lock(&installation_lock);
   int ok = installed && !sealed && !configured_delivered && configured_state == state &&
       expected->capture == configured.capture && expected->consume == configured.consume &&
       expected->finish == configured.finish && expected->destroy == configured.destroy;
   if (ok)
      configured_delivered = callback;
   pthread_mutex_unlock(&installation_lock);
   return ok ? 0 : -1;
}
HOST_ENTRY
cJSON *host_original_submission_delivered_json_v1(const host_original_submission *cap)
{
   host_original_submission_outcome out = {0};
   int accepted = -1;
   pthread_mutex_lock(&registry_lock);
   const host_original_submission *found = submissions;
   while (found && found != cap)
      found = found->next;
   if (found && found->already_delivered && found->delivered &&
       (atomic_load(&found->phase) == ACTIVE || atomic_load(&found->phase) == FINISHED) &&
       pthread_equal(found->worker, pthread_self()))
      accepted = found->delivered(found->owner, &out, found->state);
   pthread_mutex_unlock(&registry_lock);
   cJSON *result = NULL;
   if (accepted == 0 && out.json && out.length && out.length <= 32768 &&
       out.capacity == out.length + 1 &&
       memchr(out.json, 0, out.capacity) == out.json + out.length)
   {
      result = aimee_ir_json_parse_frame(out.json, out.length);
      if (!cJSON_IsObject(result))
      {
         cJSON_Delete(result);
         result = NULL;
      }
   }
   if (out.json && out.capacity <= 32769)
      cleanse(out.json, out.capacity);
   free(out.json);
   return result;
}
static host_text_status failure(host_text_status result)
{
   return result == HOST_TEXT_REFUSED || result == HOST_TEXT_UNAVAILABLE ||
                  result == HOST_TEXT_UNKNOWN || result == HOST_TEXT_UNSUPPORTED
              ? result
              : HOST_TEXT_INVALID;
}
HOST_ENTRY
void host_original_submission_finish(host_original_submission *cap, int admitted)
{
   if (!cap || atomic_load(&cap->phase) != ACTIVE || !pthread_equal(cap->worker, pthread_self()))
      return;
   int previous;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous))
      return;
   int active = ACTIVE;
   if (atomic_compare_exchange_strong(&cap->phase, &active, FINISHED))
   {
      cap->ops.finish(cap->owner, admitted == 1, cap->state);
      atomic_store(&cap->admitted, admitted == 1);
   }
   pthread_setcancelstate(previous, NULL);
}
HOST_ENTRY
void host_original_submission_destroy(host_original_submission **slot)
{
   if (!slot || !*slot)
      return;
   int previous;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous))
      return;
   host_original_submission *cap = *slot;
   pthread_mutex_lock(&registry_lock);
   host_original_submission **found = &submissions;
   while (*found && *found != cap)
      found = &(*found)->next;
   if (!*found)
   {
      *slot = NULL;
      pthread_mutex_unlock(&registry_lock);
      pthread_setcancelstate(previous, NULL);
      return;
   }
   int phase = atomic_load(&cap->phase);
   if (cap->exports || phase == CONSUMING ||
       (phase == ACTIVE && !pthread_equal(cap->worker, pthread_self())))
   {
      pthread_mutex_unlock(&registry_lock);
      pthread_setcancelstate(previous, NULL);
      return; /* Join the single owner; a concurrent export must finish first. */
   }
   *found = cap->next;
   *slot = NULL;
   pthread_mutex_unlock(&registry_lock);
   if (phase == ACTIVE)
      host_original_submission_finish(cap, 0);
   cap->ops.destroy(cap->owner, cap->state);
   memset(cap, 0, sizeof *cap);
   free(cap);
   pthread_setcancelstate(previous, NULL);
}

static void destroy_pending(void *slot)
{
   host_original_submission_destroy(slot);
}
static host_text_status capture_mode(const char *headers, const char *frame,
                                     size_t length, host_original_submission **out, int source_only)
{
   if (!out)
      return HOST_TEXT_INVALID;
   *out = NULL;
   pthread_mutex_lock(&installation_lock);
   sealed = 1;
   host_original_submission_owner ops = configured;
   void *state = configured_state;
   host_original_submission_outcome_fn outcome = configured_outcome;
   host_original_submission_outcome_fn delivered = configured_delivered;
   int enabled = installed;
   host_original_documentary_hooks documentary = configured_documentary;
   void *documentary_state = configured_documentary_state;
   pthread_mutex_unlock(&installation_lock);
   if (!enabled)
      return HOST_TEXT_PASSTHROUGH;
   if (!frame || !length || length > HOST_TEXT_MAX_INPUT + HOST_TEXT_MAX_SIDECARS + 8192u)
      return HOST_TEXT_INVALID;
   int previous;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous))
      return HOST_TEXT_UNAVAILABLE;
   cJSON *request = aimee_ir_json_parse_frame(frame, length);
   host_original_submission *cap = cJSON_IsObject(request) ? calloc(1, sizeof *cap) : NULL;
   host_text_status result = cJSON_IsObject(request) ? HOST_TEXT_UNAVAILABLE : HOST_TEXT_INVALID;
   if (cap)
   {
      cap->user_source_only = source_only;
      cap->outcome = source_only ? outcome : NULL;
      cap->delivered = delivered;
      cap->ops = ops;
      cap->state = state;
      result = ops.capture(headers, request, &cap->owner, state);
      if (result == HOST_TEXT_OK && cap->owner)
      {
         atomic_init(&cap->phase, PENDING);
         atomic_init(&cap->admitted, 0);
         cap->documentary = documentary;
         cap->documentary_state = documentary_state;
         pthread_mutex_lock(&registry_lock);
         cap->next = submissions;
         submissions = cap;
         pthread_mutex_unlock(&registry_lock);
         *out = cap;
      }
      else
      {
         if (cap->owner)
            ops.destroy(cap->owner, state);
         free(cap);
         result = failure(result);
      }
   }
   wipe_owned_dom(request);
   cJSON_Delete(request);
   pthread_cleanup_push(destroy_pending, out);
   pthread_setcancelstate(previous, NULL);
   if (previous == PTHREAD_CANCEL_ENABLE)
      pthread_testcancel();
   pthread_cleanup_pop(0);
   return result;
}
HOST_ENTRY
host_text_status host_original_submission_capture(const char *headers, const char *frame,
                                                   size_t length, host_original_submission **out)
{
   return capture_mode(headers, frame, length, out, 0);
}
HOST_ENTRY
host_text_status host_original_submission_capture_user_source(const char *headers, const char *frame,
                                                               size_t length, host_original_submission **out)
{
   /* The trusted compiled entry chooses this mode. JSON cannot select it.
    * Original owner capture/consume still resolve all actual User authority. */
   host_text_status status = capture_mode(headers, frame, length, out, 1);
   return status == HOST_TEXT_PASSTHROUGH ? HOST_TEXT_UNAVAILABLE : status;
}
HOST_ENTRY
int host_original_submission_user_source_only(const host_original_submission *cap)
{
   pthread_mutex_lock(&registry_lock);
   const host_original_submission *found = submissions;
   while (found && found != cap)
      found = found->next;
   int selected = found && found->user_source_only &&
                  atomic_load(&found->phase) == FINISHED &&
                  atomic_load(&found->admitted) && pthread_equal(found->worker, pthread_self());
   pthread_mutex_unlock(&registry_lock);
   return selected;
}
HOST_ENTRY
const unsigned aimee_original_user_source_only_contract_v1 = 1;
struct consume_cleanup
{
   host_original_submission *cap;
   host_original_submission_result *out;
};
static void consume_cancelled(void *context)
{
   struct consume_cleanup *c = context;
   host_original_submission_finish(c->cap, 0);
   host_original_submission_result_destroy(c->out);
}
HOST_ENTRY
host_text_status host_original_submission_consume(host_original_submission *cap,
                                                  const cJSON *request,
                                                  host_original_submission_result *out)
{
   if (!out)
      return HOST_TEXT_INVALID;
   memset(out, 0, sizeof *out);
   if (!cap || !request)
      return HOST_TEXT_UNAVAILABLE;
   int previous;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous))
      return HOST_TEXT_UNAVAILABLE;
   int pending = PENDING;
   host_text_status result = HOST_TEXT_REFUSED;
   if (atomic_compare_exchange_strong(&cap->phase, &pending, CONSUMING))
   {
      cap->worker = pthread_self();
      result = cap->ops.consume(cap->owner, request, out, cap->state);
      /* Shape/commitment checks are generic. The private owner must validate
       * UTF8, sensitivity policy and its exact request/context binding. */
      char actual[65];
      const char *expected =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(out->attachment, "text_sha256"));
      if (result == HOST_TEXT_ALREADY_DELIVERED && cap->delivered &&
          !out->text && !out->text_len && !out->text_capacity && !out->attachment)
      {
         cap->already_delivered = 1;
         atomic_store(&cap->phase, ACTIVE);
      }
      else if (result == HOST_TEXT_OK && out->text && out->text[0] && out->text_len > 0 &&
          out->text_len <= HOST_TEXT_MAX_OUTPUT && out->text_capacity == out->text_len + 1 &&
          memchr(out->text, 0, out->text_capacity) == out->text + out->text_len &&
          aimee_ir_input_attachment_shape_valid(out->attachment) && expected &&
          !aimee_sha256_hex(out->text, out->text_len, actual) && !strcmp(actual, expected))
         atomic_store(&cap->phase, ACTIVE);
      else
      {
         cap->ops.finish(cap->owner, 0, cap->state);
         atomic_store(&cap->phase, FINISHED);
         host_original_submission_result_destroy(out);
         result = failure(result);
      }
   }
   struct consume_cleanup cleanup = {cap, out};
   pthread_cleanup_push(consume_cancelled, &cleanup);
   pthread_setcancelstate(previous, NULL);
   if (previous == PTHREAD_CANCEL_ENABLE)
      pthread_testcancel();
   pthread_cleanup_pop(0);
   return result;
}
HOST_ENTRY
host_original_submission **host_original_submission_pending_bind(host_original_submission **slot)
{
   host_original_submission **previous = pending_slot;
   pending_slot = slot;
   return previous;
}
HOST_ENTRY
host_original_submission *host_original_submission_pending_take(void)
{
   if (!pending_slot)
      return NULL;
   host_original_submission *cap = *pending_slot;
   *pending_slot = NULL;
   return cap;
}

HOST_ENTRY
int host_original_submission_install_documentary_hooks(const host_original_documentary_hooks *hooks,
                                                       void *state)
{
   if (!hooks || hooks->abi_version != 1 || !hooks->retain || !hooks->release)
      return -1;
   pthread_mutex_lock(&installation_lock);
   int ok = !sealed && !documentary_installed;
   if (ok)
   {
      configured_documentary = *hooks;
      configured_documentary_state = state;
      documentary_installed = 1;
   }
   pthread_mutex_unlock(&installation_lock);
   return ok ? 0 : -1;
}
HOST_ENTRY
int host_original_submission_documentary_required(void)
{
   pthread_mutex_lock(&installation_lock);
   int required = documentary_installed;
   pthread_mutex_unlock(&installation_lock);
   return required;
}
HOST_ENTRY
const void *host_original_documentary_borrow(const host_original_documentary *cap)
{
   pthread_mutex_lock(&registry_lock);
   host_original_documentary *found = documents;
   while (found && found != cap)
      found = found->next;
   const void *document = found ? found->private_document : NULL;
   pthread_mutex_unlock(&registry_lock);
   return document;
}
HOST_ENTRY
int host_original_documentary_retain(const host_original_documentary *cap,
                                     host_original_documentary **out)
{
   if (!out || *out)
      return -1;
   pthread_mutex_lock(&registry_lock);
   host_original_documentary *found = documents;
   while (found && found != cap)
      found = found->next;
   int ok = found && found->references < UINT_MAX;
   if (ok)
   {
      found->references++;
      *out = found;
   }
   pthread_mutex_unlock(&registry_lock);
   return ok ? 0 : -1;
}
HOST_ENTRY
void host_original_documentary_release(host_original_documentary **slot)
{
   if (!slot || !*slot)
      return;
   int previous;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous))
      return;
   host_original_documentary *cap = *slot;
   *slot = NULL;
   pthread_mutex_lock(&registry_lock);
   host_original_documentary **found = &documents;
   while (*found && *found != cap)
      found = &(*found)->next;
   int destroy = *found && --cap->references == 0;
   if (destroy)
      *found = cap->next;
   pthread_mutex_unlock(&registry_lock);
   if (destroy)
   {
      cap->hooks.release(cap->private_document, cap->state);
      memset(cap, 0, sizeof *cap);
      free(cap);
   }
   pthread_setcancelstate(previous, NULL);
   if (previous == PTHREAD_CANCEL_ENABLE)
      pthread_testcancel();
}
static void documentary_cancelled(void *slot)
{
   host_original_documentary_release(slot);
}
HOST_ENTRY
host_text_status host_original_submission_documentary_retain(host_original_submission *cap,
                                                             host_original_documentary **out)
{
   if (!out || *out)
      return HOST_TEXT_INVALID;
   int previous;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous))
      return HOST_TEXT_UNAVAILABLE;
   pthread_mutex_lock(&registry_lock);
   host_original_submission *found = submissions;
   while (found && found != cap)
      found = found->next;
   int eligible = found && atomic_load(&found->phase) == FINISHED &&
                  atomic_load(&found->admitted) && pthread_equal(found->worker, pthread_self()) &&
                  found->exports < UINT_MAX;
   int available = eligible && found->documentary.abi_version == 1;
   if (available)
      found->exports++;
   pthread_mutex_unlock(&registry_lock);
   host_text_status status = eligible ? HOST_TEXT_UNAVAILABLE : HOST_TEXT_REFUSED;
   if (available)
   {
      void *private_document = NULL;
      int retained = cap->documentary.retain(cap->owner, &private_document, cap->documentary_state);
      host_original_documentary *document =
          !retained && private_document ? calloc(1, sizeof *document) : NULL;
      if (document)
      {
         document->hooks = cap->documentary;
         document->private_document = private_document;
         document->state = cap->documentary_state;
         document->references = 1;
         pthread_mutex_lock(&registry_lock);
         document->next = documents;
         documents = document;
         pthread_mutex_unlock(&registry_lock);
         *out = document;
         status = HOST_TEXT_OK;
      }
      else
      {
         if (private_document)
            cap->documentary.release(private_document, cap->documentary_state);
         status = HOST_TEXT_UNAVAILABLE;
      }
      pthread_mutex_lock(&registry_lock);
      cap->exports--;
      pthread_mutex_unlock(&registry_lock);
   }
   pthread_cleanup_push(documentary_cancelled, out);
   pthread_setcancelstate(previous, NULL);
   if (previous == PTHREAD_CANCEL_ENABLE)
      pthread_testcancel();
   pthread_cleanup_pop(0);
   return status;
}
