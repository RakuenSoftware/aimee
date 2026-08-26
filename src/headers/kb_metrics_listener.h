/* Dedicated, opt-in Prometheus scrape listener for aimee-kb. */
#pragma once

typedef struct
{
   const char *endpoint;
   const char *tls_certificate_file;
   const char *tls_key_file;
   const char *tls_client_ca_file;
   const char *bearer_token_file;
   const char *bearer_token_hash;
} kb_metrics_listener_config_t;

/* Empty endpoint disables the listener. Supported forms are tcp://host:port
 * and unix:///absolute/path. Non-loopback TCP requires TLS and either mTLS or
 * a bearer. Private keys and raw-token files must be owner-only. */
int kb_metrics_listener_start(const kb_metrics_listener_config_t *config);

/* Stop the listener thread and remove its owned Unix socket, if any. */
void kb_metrics_listener_stop(void);
