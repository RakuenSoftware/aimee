/* git_host_cred.c — per-host git credentials in the server vault. See header. */
#include "git_host_cred.h"

#include "util_url.h"      /* util_url_normalize */
#include "vault_service.h" /* vault_service_set_server / get_server_wrap / delete */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GIT_HOST_AGENT       "git"
#define GIT_HOST_CRED_PREFIX "host:"

/* Validate a host: non-empty, fits, and only the characters a hostname (+ an
 * optional :port the normalizer already strips) can contain. Rejects anything
 * that could smuggle a separator into the vault cred key. */
static int host_valid(const char *host)
{
   if (!host || !host[0])
      return 0;
   size_t n = strlen(host);
   if (n >= GIT_HOST_MAX - sizeof(GIT_HOST_CRED_PREFIX))
      return 0;
   for (const char *p = host; *p; p++)
   {
      char c = *p;
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '-' || c == '_'))
         return 0;
   }
   return 1;
}

/* Build the vault cred key "host:<host>" into buf. Returns 0, or -1 on overflow. */
static int cred_key(const char *host, char *buf, size_t cap)
{
   int n = snprintf(buf, cap, "%s%s", GIT_HOST_CRED_PREFIX, host);
   return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

int git_host_from_url(const char *url, char *out, size_t out_len)
{
   if (!url || !out || out_len == 0)
      return 0;
   out[0] = '\0';
   char *norm = util_url_normalize(url); /* any form -> https://host/path, host lowercased */
   if (!norm)
      return 0;
   int ok = 0;
   /* norm is "https://<host>/<path>"; host is between the authority slashes. */
   const char *p = strstr(norm, "://");
   if (p)
   {
      p += 3;
      const char *slash = strchr(p, '/');
      size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
      if (hlen > 0 && hlen < out_len)
      {
         memcpy(out, p, hlen);
         out[hlen] = '\0';
         ok = host_valid(out);
         if (!ok)
            out[0] = '\0';
      }
   }
   free(norm);
   return ok;
}

int git_host_cred_set(const char *host, const char *token)
{
   if (!host_valid(host) || !token || !token[0])
      return -1;
   char key[GIT_HOST_MAX];
   if (cred_key(host, key, sizeof(key)) != 0)
      return -1;
   return vault_service_set_server(GIT_HOST_AGENT, key, token) == VAULT_OK ? 0 : -1;
}

int git_host_cred_get(const char *host, char *out, size_t out_len)
{
   if (out && out_len)
      out[0] = '\0';
   if (!host_valid(host) || !out || out_len == 0)
      return -1;
   char key[GIT_HOST_MAX];
   if (cred_key(host, key, sizeof(key)) != 0)
      return -1;
   vault_status_t st =
       vault_service_get_server_wrap(VAULT_SERVER_PRINCIPAL, GIT_HOST_AGENT, key, out, out_len);
   if (st == VAULT_OK && out[0])
      return 1;
   if (st == VAULT_NO_ENTRY || st == VAULT_OK)
      return 0; /* absent (or empty) -> no credential */
   return -1;   /* genuine error */
}

int git_host_cred_for_url(const char *url, char *out, size_t out_len)
{
   char host[GIT_HOST_MAX];
   if (!git_host_from_url(url, host, sizeof(host)))
   {
      if (out && out_len)
         out[0] = '\0';
      return 0;
   }
   return git_host_cred_get(host, out, out_len);
}

int git_host_cred_delete(const char *host)
{
   if (!host_valid(host))
      return -1;
   char key[GIT_HOST_MAX];
   if (cred_key(host, key, sizeof(key)) != 0)
      return -1;
   vault_status_t st = vault_service_delete(VAULT_SERVER_PRINCIPAL, GIT_HOST_AGENT, key);
   return (st == VAULT_OK || st == VAULT_NO_ENTRY) ? 0 : -1;
}
