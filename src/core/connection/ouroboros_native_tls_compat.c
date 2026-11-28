/* Thin compatibility adapter forwarding aimee/core/connection/native_tls.h
 * to ouroboros-connection, bridging AIMEE_HOME / AIMEE_PROFILE and legacy
 * AIMEE_TLS_* environment variables. Kept in its own translation unit so
 * non-TLS binaries linking libaimee-core-connection.a do not pull TLS. */
#include <aimee/core/connection/native_tls.h>
#include <ouroboros/connection/native_tls.h>

#include <stdio.h>
#include <stdlib.h>

#if defined(_MSC_VER)
#define AIMEE_TLS_THREAD_LOCAL __declspec(thread)
#else
#define AIMEE_TLS_THREAD_LOCAL _Thread_local
#endif

static const char *aimee_default_tls_home(void)
{
   static AIMEE_TLS_THREAD_LOCAL char path[4096];
   const char *override = getenv("AIMEE_HOME");
   if (override && override[0])
   {
      int n = snprintf(path, sizeof(path), "%s", override);
      return n > 0 && (size_t)n < sizeof(path) ? path : NULL;
   }
   const char *home = getenv("HOME");
#ifdef _WIN32
   if (!home || !home[0])
      home = getenv("USERPROFILE");
#endif
   if (!home || !home[0])
      return NULL;
   const char *profile = getenv("AIMEE_PROFILE");
   int n = profile && profile[0]
               ? snprintf(path, sizeof(path), "%s/.config/aimee/profiles/%s", home, profile)
               : snprintf(path, sizeof(path), "%s/.config/aimee", home);
   return n > 0 && (size_t)n < sizeof(path) ? path : NULL;
}

static void aimee_tls_bridge_env(void)
{
   ouro_native_tls_set_home_resolver(aimee_default_tls_home);
   const char *insec = getenv("AIMEE_TLS_INSECURE");
   if (insec && insec[0] && !getenv("OURO_TLS_INSECURE"))
   {
#ifdef _WIN32
      _putenv_s("OURO_TLS_INSECURE", insec);
#else
      setenv("OURO_TLS_INSECURE", insec, 0);
#endif
   }
   const char *p12 = getenv("AIMEE_TLS_CLIENT_P12_PASS");
   if (p12 && p12[0] && !getenv("OURO_TLS_CLIENT_P12_PASS"))
   {
#ifdef _WIN32
      _putenv_s("OURO_TLS_CLIENT_P12_PASS", p12);
#else
      setenv("OURO_TLS_CLIENT_P12_PASS", p12, 0);
#endif
   }
}

aimee_tls_t *aimee_tls_connect(int fd, const char *host)
{
   aimee_tls_bridge_env();
   return (aimee_tls_t *)ouro_native_tls_connect(fd, host);
}

aimee_tls_t *aimee_tls_connect_home(int fd, const char *host, const char *home)
{
   aimee_tls_bridge_env();
   return (aimee_tls_t *)ouro_native_tls_connect_home(fd, host, home);
}

int aimee_tls_write_all(aimee_tls_t *t, const void *buf, size_t len)
{
   return ouro_native_tls_write_all((ouro_native_tls_t *)t, buf, len);
}

long aimee_tls_read(aimee_tls_t *t, void *buf, size_t len)
{
   return ouro_native_tls_read((ouro_native_tls_t *)t, buf, len);
}

void aimee_tls_free(aimee_tls_t *t)
{
   ouro_native_tls_free((ouro_native_tls_t *)t);
}

int aimee_tls_client_cert_eligible(const char *home, char *crt, size_t crt_n, char *key,
                                   size_t key_n)
{
   return ouro_native_tls_client_cert_eligible(home, crt, crt_n, key, key_n);
}

void aimee_tls_suppress_client_cert(int on)
{
   ouro_native_tls_suppress_client_cert(on);
}

int aimee_tls_fetch_peer_cert(const char *host, const char *port, char **pem_out, char *fp_out,
                              size_t fp_n)
{
   return ouro_native_tls_fetch_peer_cert(host, port, pem_out, fp_out, fp_n);
}
