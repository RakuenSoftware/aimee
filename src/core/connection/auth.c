#include <aimee/core/connection/auth.h>

#include <stdio.h>
#include <string.h>

static int ascii_lower(int ch)
{
   return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
}

static int ascii_equal_n(const char *left, const char *right, size_t length)
{
   for (size_t i = 0; i < length; i++)
   {
      if (!left[i] || !right[i])
         return 0;
      if (ascii_lower((unsigned char)left[i]) != ascii_lower((unsigned char)right[i]))
         return 0;
   }
   return 1;
}

int aimee_core_credential_equal(const char *left, const char *right)
{
   size_t left_len = left ? strlen(left) : 0;
   size_t right_len = right ? strlen(right) : 0;
   size_t length = left_len > right_len ? left_len : right_len;
   size_t diff = left_len ^ right_len;
   for (size_t i = 0; i < length; i++)
   {
      unsigned char x = i < left_len ? (unsigned char)left[i] : 0;
      unsigned char y = i < right_len ? (unsigned char)right[i] : 0;
      diff |= (size_t)(x ^ y);
   }
   return diff == 0;
}

static int credential_byte(unsigned char ch)
{
   return ch > 0x20 && ch != 0x7f;
}

int aimee_core_bearer_token_span(const char *value, size_t value_len, const char **token_out,
                                 size_t *token_len_out)
{
   static const char scheme[] = "Bearer";
   const size_t scheme_len = sizeof(scheme) - 1;
   if (token_out)
      *token_out = NULL;
   if (token_len_out)
      *token_len_out = 0;
   if (!value || value_len <= scheme_len + 1 || !ascii_equal_n(value, scheme, scheme_len) ||
       value[scheme_len] != ' ')
      return -1;

   const char *token = value + scheme_len + 1;
   const char *end = value + value_len;
   while (token < end && *token == ' ')
      token++;
   if (token == end)
      return -1;
   for (const char *p = token; p < end; p++)
      if (!credential_byte((unsigned char)*p))
         return -1;
   if (token_out)
      *token_out = token;
   if (token_len_out)
      *token_len_out = (size_t)(end - token);
   return 0;
}

const char *aimee_core_bearer_token(const char *authorization_value)
{
   const char *token = NULL;
   if (!authorization_value ||
       aimee_core_bearer_token_span(authorization_value, strlen(authorization_value), &token,
                                    NULL) != 0)
      return NULL;
   return token;
}

int aimee_core_bearer_value(char *out, size_t out_size, const char *token)
{
   if (!out || out_size == 0 || !token || !*token)
      return -1;
   for (const char *p = token; *p; p++)
      if (!credential_byte((unsigned char)*p))
         return -1;
   int written = snprintf(out, out_size, "Bearer %s", token);
   return written > 0 && (size_t)written < out_size ? 0 : -1;
}

int aimee_core_host_is_loopback(const char *host)
{
   if (!host || !*host)
      return 0;
   size_t length = strlen(host);
   if ((length == 9 && ascii_equal_n(host, "localhost", 9)) || strcmp(host, "::1") == 0 ||
       strcmp(host, "[::1]") == 0)
      return 1;
   return strncmp(host, "127.", 4) == 0;
}

int aimee_core_would_leak_credential(int transport_is_secure, const char *host,
                                     const char *credential)
{
   return credential && *credential && !transport_is_secure && !aimee_core_host_is_loopback(host);
}
