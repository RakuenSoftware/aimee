#ifndef AIMEE_KB_MGMT_STATUS_CLIENT_H
#define AIMEE_KB_MGMT_STATUS_CLIENT_H

#include "kb_management_cert_lifecycle.h"
#include "kb_management_health_exchange.h"

#include <stddef.h>
#include <stdint.h>

typedef struct
{
   const char *endpoint; /* Root/operator configuration only. */
   const char *ca_pem;
   const char *leaf_pin;
   const char *secondary_leaf_pin;
} kb_mgmt_status_client_config_t;

kb_management_health_result_t kb_mgmt_status_client_issue(const kb_mgmt_status_client_config_t *,
                                                          const kb_management_cert_bundle_t *,
                                                          const char *, size_t, uint64_t, char *,
                                                          size_t, int *);
kb_management_health_result_t kb_mgmt_status_client_adapter(void *,
                                                            const kb_management_cert_bundle_t *,
                                                            const char *, size_t, uint64_t, char *,
                                                            size_t, int *);
int kb_mgmt_status_client_pin_matches(const char *, const char *, const char *);

#endif
