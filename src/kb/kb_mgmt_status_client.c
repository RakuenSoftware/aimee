#include "kb_mgmt_status_client.h"

#include "kb_mgmt_client.h"
#include "kb_mgmt_endpoint.h"
#include "kb_tls.h"

#include <openssl/crypto.h>
#include <string.h>
#include <time.h>

static uint64_t monotonic_ms(void)
{
   struct timespec ts;
   return clock_gettime(CLOCK_MONOTONIC, &ts) == 0
              ? (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000
              : UINT64_MAX;
}

static int pin_valid(const char *pin)
{
   if (!pin || strlen(pin) != 64)
      return 0;
   for (size_t i = 0; i < 64; i++)
      if (!((pin[i] >= '0' && pin[i] <= '9') || (pin[i] >= 'a' && pin[i] <= 'f')))
         return 0;
   return 1;
}

int kb_mgmt_status_client_pin_matches(const char *actual, const char *primary,
                                      const char *secondary)
{
   return pin_valid(actual) && pin_valid(primary) && (!secondary || pin_valid(secondary)) &&
          (CRYPTO_memcmp(actual, primary, 64) == 0 ||
           (secondary && CRYPTO_memcmp(actual, secondary, 64) == 0));
}

kb_management_health_result_t kb_mgmt_status_client_issue(const kb_mgmt_status_client_config_t *c,
                                                          const kb_management_cert_bundle_t *bundle,
                                                          const char *request, size_t request_len,
                                                          uint64_t deadline, char *response,
                                                          size_t cap, int *status)
{
   if (response && cap)
      memset(response, 0, cap);
   if (!c || !bundle || !request || request_len != strlen(request) || !request_len ||
       request_len > 1024 || !response || cap < 2 || cap > KB_MGMT_STATUS_JSON_MAX + 1 || !status ||
       !pin_valid(c->leaf_pin) || (c->secondary_leaf_pin && !pin_valid(c->secondary_leaf_pin)) ||
       kb_mgmt_endpoint_validate(c->endpoint) || !c->ca_pem || !bundle->leaf_pem_len ||
       !bundle->key_pem_len || monotonic_ms() >= deadline)
      return KB_MANAGEMENT_HEALTH_INVALID;
   kb_mgmt_client_session_t session;
   if (kb_mgmt_client_session_open_deadline(&session, c->endpoint, c->ca_pem, bundle->leaf_pem,
                                            bundle->key_pem, NULL, NULL, NULL, deadline, 1))
      return KB_MANAGEMENT_HEALTH_UNAVAILABLE;
   char actual[65] = {0};
   int pinned = kb_tls_peer_fingerprint(session.ssl, actual, sizeof(actual)) == 0 &&
                kb_mgmt_status_client_pin_matches(actual, c->leaf_pin, c->secondary_leaf_pin);
   int rc = -1;
   if (pinned && monotonic_ms() < deadline)
      rc = kb_mgmt_client_session_request_deadline(&session, "POST", "/v1/management/status",
                                                   request, NULL, deadline, response, cap, status);
   kb_mgmt_client_session_close(&session);
   OPENSSL_cleanse(actual, sizeof(actual));
   if (!pinned)
      return KB_MANAGEMENT_HEALTH_INTEGRITY;
   if (rc || monotonic_ms() >= deadline)
      return KB_MANAGEMENT_HEALTH_UNAVAILABLE;
   return *status == 200 || *status == 400 || *status == 403 || *status == 409 || *status == 503
              ? KB_MANAGEMENT_HEALTH_OK
              : KB_MANAGEMENT_HEALTH_INTEGRITY;
}

kb_management_health_result_t
kb_mgmt_status_client_adapter(void *ctx, const kb_management_cert_bundle_t *bundle,
                              const char *request, size_t request_len, uint64_t deadline,
                              char *response, size_t cap, int *status)
{
   return kb_mgmt_status_client_issue(ctx, bundle, request, request_len, deadline, response, cap,
                                      status);
}
