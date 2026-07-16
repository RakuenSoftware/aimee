/* util_url.c: canonical URL handling for project/workspace identity.
 * See headers/util_url.h for the pipeline contract.
 */
#include "util_url.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strncasecmp */

int util_url_host_is_case_insensitive(const char *host)
{
   if (!host || !*host)
      return 0;

   static const char *const exact[] = {"github.com", "gitlab.com", "bitbucket.org"};
   static const char *const suffixes[] = {".github.com", ".gitlab.com", ".bitbucket.org"};

   for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); i++)
   {
      if (strcmp(host, exact[i]) == 0)
         return 1;
   }

   size_t hl = strlen(host);
   for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++)
   {
      size_t sl = strlen(suffixes[i]);
      if (hl > sl && strcmp(host + hl - sl, suffixes[i]) == 0)
         return 1;
   }
   return 0;
}

static int scheme_recognized(const char *url, size_t len)
{
   return (len == 5 && strncasecmp(url, "https", 5) == 0) ||
          (len == 4 && strncasecmp(url, "http", 4) == 0) ||
          (len == 3 && strncasecmp(url, "ssh", 3) == 0) ||
          (len == 3 && strncasecmp(url, "git", 3) == 0);
}

int util_url_is_ssh(const char *url)
{
   if (!url || !*url)
      return 0;

   const char *proto = strstr(url, "://");
   if (proto)
   {
      size_t scheme_len = (size_t)(proto - url);
      /* Only ssh:// authenticates with a key; git:// is anonymous. */
      return scheme_len == 3 && strncasecmp(url, "ssh", 3) == 0;
   }

   /* scp-like: user@host:path — an '@' must precede the first ':'. */
   const char *colon = strchr(url, ':');
   if (!colon)
      return 0;
   const char *at = memchr(url, '@', (size_t)(colon - url));
   return at != NULL && at > url;
}

char *util_url_normalize(const char *url)
{
   if (!url || !*url)
      return NULL;

   const char *host_start = NULL;
   const char *host_end = NULL; /* exclusive */
   const char *path_start = NULL;

   const char *colon = strchr(url, ':');
   const char *proto = strstr(url, "://");

   if (proto)
   {
      size_t scheme_len = (size_t)(proto - url);
      if (!scheme_recognized(url, scheme_len))
         return NULL;

      const char *authority = proto + 3;
      const char *first_slash = strchr(authority, '/');
      if (!first_slash || first_slash == authority)
         return NULL; /* must have a host before the path */

      const char *at = memchr(authority, '@', (size_t)(first_slash - authority));
      host_start = at ? at + 1 : authority;
      host_end = first_slash;
      path_start = first_slash + 1;
   }
   else if (colon)
   {
      /* scp-like: user@host:path (no '://', must have '@' before ':') */
      const char *at = memchr(url, '@', (size_t)(colon - url));
      if (!at)
         return NULL;
      host_start = at + 1;
      host_end = colon;
      path_start = colon + 1;
   }
   else
   {
      return NULL;
   }

   if (host_start >= host_end)
      return NULL;

   /* Lowercase host, then strip port if present. */
   size_t raw_host_len = (size_t)(host_end - host_start);
   char *host = malloc(raw_host_len + 1);
   if (!host)
      return NULL;
   for (size_t i = 0; i < raw_host_len; i++)
      host[i] = (char)tolower((unsigned char)host_start[i]);
   host[raw_host_len] = '\0';
   char *port_colon = strchr(host, ':');
   if (port_colon)
      *port_colon = '\0';
   if (*host == '\0')
   {
      free(host);
      return NULL;
   }

   int lowercase_path = util_url_host_is_case_insensitive(host);

   /* Normalize path: collapse '//' runs, optional case-lower, strip trailing '/',
    * then strip a single trailing '.git'. */
   size_t in_len = strlen(path_start);
   char *path = malloc(in_len + 1);
   if (!path)
   {
      free(host);
      return NULL;
   }
   size_t pi = 0;
   int prev_slash = 1; /* treat start as "just after a slash" so leading runs collapse */
   for (size_t i = 0; i < in_len; i++)
   {
      char c = path_start[i];
      if (c == '/')
      {
         if (prev_slash)
            continue;
         path[pi++] = '/';
         prev_slash = 1;
      }
      else
      {
         path[pi++] = lowercase_path ? (char)tolower((unsigned char)c) : c;
         prev_slash = 0;
      }
   }
   while (pi > 0 && path[pi - 1] == '/')
      pi--;
   if (pi >= 4 && memcmp(path + pi - 4, ".git", 4) == 0)
      pi -= 4;
   path[pi] = '\0';

   if (pi == 0)
   {
      free(host);
      free(path);
      return NULL;
   }

   size_t needed = strlen("https://") + strlen(host) + 1 /* '/' */ + pi + 1 /* NUL */;
   char *out = malloc(needed);
   if (!out)
   {
      free(host);
      free(path);
      return NULL;
   }
   snprintf(out, needed, "https://%s/%s", host, path);

   free(host);
   free(path);
   return out;
}

char *util_url_workspace_parent(const char *project_url)
{
   if (!project_url)
      return NULL;
   if (strncmp(project_url, "local:", 6) == 0)
      return NULL;
   if (strncmp(project_url, "https://", 8) != 0)
      return NULL;

   const char *host_start = project_url + 8;
   const char *first_slash = strchr(host_start, '/');
   if (!first_slash)
      return NULL;
   const char *last_slash = strrchr(host_start, '/');
   if (!last_slash || last_slash <= first_slash)
      return NULL; /* already at host root (host/leaf) */

   size_t parent_len = (size_t)(last_slash - project_url);
   char *out = malloc(parent_len + 1);
   if (!out)
      return NULL;
   memcpy(out, project_url, parent_len);
   out[parent_len] = '\0';
   return out;
}
