/* proxy_bootstrap.h: Enterprise proxy and CA bundle detection from environment.
 *
 * Reads standard proxy environment variables (HTTPS_PROXY, NO_PROXY,
 * SSL_CERT_FILE, etc.) and exposes a singleton that the HTTP client and
 * subprocess layer use to route traffic and verify certificates. */
#ifndef DEC_PROXY_BOOTSTRAP_H
#define DEC_PROXY_BOOTSTRAP_H 1

#define PROXY_HOST_LEN      256
#define PROXY_AUTH_LEN      256
#define PROXY_NO_PROXY_LEN  4096
#define PROXY_CA_BUNDLE_LEN 1024

/* Parsed proxy endpoint */
typedef struct
{
   char host[PROXY_HOST_LEN]; /* proxy hostname */
   int port;                  /* proxy port */
   char auth[PROXY_AUTH_LEN]; /* "user:pass" or empty */
} proxy_config_t;

/* Full proxy context derived from the environment */
typedef struct
{
   proxy_config_t https_proxy;          /* resolved HTTPS proxy */
   int has_proxy;                       /* 1 if a proxy was found */
   char no_proxy[PROXY_NO_PROXY_LEN];   /* merged NO_PROXY list */
   char ca_bundle[PROXY_CA_BUNDLE_LEN]; /* CA bundle path, or empty */
} proxy_bootstrap_t;

/* Parse a proxy URL string into a proxy_config_t.
 * Accepts "http://host:port", "http://user:pass@host:port", or "host:port".
 * Returns 0 on success, -1 on failure. */
int proxy_parse_url(const char *url, proxy_config_t *out);

/* Initialize pb from environment variables.
 * Variables checked (first non-empty wins for proxy):
 *   HTTPS_PROXY, https_proxy, HTTP_PROXY, http_proxy
 * NO_PROXY/no_proxy is merged with a built-in list of safe hosts.
 * CA bundle path resolved from (first non-empty wins):
 *   SSL_CERT_FILE, REQUESTS_CA_BUNDLE, NODE_EXTRA_CA_CERTS, CURL_CA_BUNDLE */
void proxy_bootstrap_init(proxy_bootstrap_t *pb);

/* Returns 1 if requests to `host` should be routed through the proxy.
 * Returns 0 if the host matches an entry in pb->no_proxy (exact match,
 * leading-dot domain suffix, or plain domain suffix). */
int proxy_should_use(const proxy_bootstrap_t *pb, const char *host);

/* Initialize the process-wide singleton from the environment.
 * Safe to call multiple times (only the first call takes effect).
 * Also sets HTTPS_PROXY / NO_PROXY in the environment so that subprocesses
 * inherit the merged configuration. */
void proxy_bootstrap_init_global(void);

/* Return the process-wide proxy bootstrap singleton.
 * Returns NULL before proxy_bootstrap_init_global() is called. */
const proxy_bootstrap_t *proxy_get(void);

#endif /* DEC_PROXY_BOOTSTRAP_H */
