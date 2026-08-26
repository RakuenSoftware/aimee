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

/* Resolve service-specific settings over generic settings, then apply any
 * observability command-line overrides. parse_arg returns 1 when handled. */
void kb_metrics_listener_config_from_env(kb_metrics_listener_config_t *config);
int kb_metrics_listener_config_parse_arg(kb_metrics_listener_config_t *config, const char *arg);
void kb_metrics_listener_print_usage(void);

/* Empty endpoint disables the listener. Supported forms are tcp://host:port
 * and unix:///absolute/path. Non-loopback TCP requires TLS and either mTLS or
 * a bearer. Private keys and raw-token files must be owner-only. */
int kb_metrics_listener_start(const kb_metrics_listener_config_t *config);

/* Load the legacy hashed token from the runtime-secret store, start the
 * listener, and wipe the transient copy before returning. */
int kb_metrics_listener_start_from_runtime(kb_metrics_listener_config_t *config);

/* Stop the listener thread and remove its owned Unix socket, if any. */
void kb_metrics_listener_stop(void);
