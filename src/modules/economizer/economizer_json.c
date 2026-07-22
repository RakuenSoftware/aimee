#include "economizer_json.h"

#include <stdlib.h>
#include <string.h>

typedef struct
{
   const uint8_t *s;
   size_t n;
   size_t p;
} parser_t;

typedef struct
{
   uint8_t *s;
   size_t n;
} json_key_t;

static int ws(uint8_t c)
{
   return c == 0x20 || c == '\t' || c == '\n' || c == '\r';
}

static void skip_ws(parser_t *p)
{
   while (p->p < p->n && ws(p->s[p->p]))
      p->p++;
}

static int hexval(uint8_t c)
{
   if (c >= '0' && c <= '9')
      return c - '0';
   if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
   if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
   return -1;
}

static int hex4(parser_t *p, uint32_t *out)
{
   if (p->n - p->p < 4)
      return -1;
   uint32_t v = 0;
   for (int i = 0; i < 4; i++)
   {
      int h = hexval(p->s[p->p++]);
      if (h < 0)
         return -1;
      v = (v << 4) | (uint32_t)h;
   }
   *out = v;
   return 0;
}

static int utf8_sequence(const uint8_t *s, size_t n, size_t *used)
{
   if (!n)
      return -1;
   uint8_t a = s[0];
   if (a < 0x80)
   {
      *used = 1;
      return 0;
   }
   if (a >= 0xc2 && a <= 0xdf && n >= 2 && (s[1] & 0xc0) == 0x80)
      *used = 2;
   else if (a == 0xe0 && n >= 3 && s[1] >= 0xa0 && s[1] <= 0xbf && (s[2] & 0xc0) == 0x80)
      *used = 3;
   else if (a >= 0xe1 && a <= 0xec && n >= 3 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80)
      *used = 3;
   else if (a == 0xed && n >= 3 && s[1] >= 0x80 && s[1] <= 0x9f && (s[2] & 0xc0) == 0x80)
      *used = 3;
   else if (a >= 0xee && a <= 0xef && n >= 3 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80)
      *used = 3;
   else if (a == 0xf0 && n >= 4 && s[1] >= 0x90 && s[1] <= 0xbf && (s[2] & 0xc0) == 0x80 &&
            (s[3] & 0xc0) == 0x80)
      *used = 4;
   else if (a >= 0xf1 && a <= 0xf3 && n >= 4 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 &&
            (s[3] & 0xc0) == 0x80)
      *used = 4;
   else if (a == 0xf4 && n >= 4 && s[1] >= 0x80 && s[1] <= 0x8f && (s[2] & 0xc0) == 0x80 &&
            (s[3] & 0xc0) == 0x80)
      *used = 4;
   else
      return -1;
   return 0;
}

static int append_utf8(uint8_t *dst, size_t cap, size_t *len, uint32_t cp)
{
   size_t need = cp <= 0x7f ? 1 : cp <= 0x7ff ? 2 : cp <= 0xffff ? 3 : 4;
   if (*len > cap || cap - *len < need || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
      return -1;
   if (need == 1)
      dst[(*len)++] = (uint8_t)cp;
   else if (need == 2)
   {
      dst[(*len)++] = (uint8_t)(0xc0 | (cp >> 6));
      dst[(*len)++] = (uint8_t)(0x80 | (cp & 0x3f));
   }
   else if (need == 3)
   {
      dst[(*len)++] = (uint8_t)(0xe0 | (cp >> 12));
      dst[(*len)++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
      dst[(*len)++] = (uint8_t)(0x80 | (cp & 0x3f));
   }
   else
   {
      dst[(*len)++] = (uint8_t)(0xf0 | (cp >> 18));
      dst[(*len)++] = (uint8_t)(0x80 | ((cp >> 12) & 0x3f));
      dst[(*len)++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
      dst[(*len)++] = (uint8_t)(0x80 | (cp & 0x3f));
   }
   return 0;
}

static int key_reserve(uint8_t **buf, size_t *cap, size_t len, size_t add)
{
   if (len > ECON_JSON_MAX_INPUT || add > ECON_JSON_MAX_INPUT - len)
      return -1;
   if (*cap >= len + add)
      return 0;
   size_t next = *cap ? *cap : 32;
   while (next < len + add)
   {
      if (next > ECON_JSON_MAX_INPUT / 2)
      {
         next = ECON_JSON_MAX_INPUT;
         break;
      }
      next *= 2;
   }
   uint8_t *grown = realloc(*buf, next);
   if (!grown)
      return -1;
   *buf = grown;
   *cap = next;
   return 0;
}

/* Validate a string and optionally decode an object name for duplicate checks. */
static econ_json_result_t parse_string(parser_t *p, json_key_t *decoded)
{
   if (p->p >= p->n || p->s[p->p++] != '"')
      return ECON_JSON_INVALID_SYNTAX;
   uint8_t *buf = NULL;
   size_t len = 0, cap = 0;
   while (p->p < p->n)
   {
      uint8_t c = p->s[p->p++];
      if (c == '"')
      {
         if (decoded)
         {
            decoded->s = buf;
            decoded->n = len;
         }
         return ECON_JSON_OK;
      }
      if (c < 0x20)
         break;
      if (c == '\\')
      {
         if (p->p >= p->n)
            break;
         uint8_t e = p->s[p->p++];
         uint32_t cp;
         if (e == 'u')
         {
            if (hex4(p, &cp) != 0)
               break;
            if (cp >= 0xd800 && cp <= 0xdbff)
            {
               uint32_t low;
               if (p->n - p->p < 6 || p->s[p->p++] != '\\' || p->s[p->p++] != 'u' ||
                   hex4(p, &low) != 0 || low < 0xdc00 || low > 0xdfff)
                  break;
               cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
            }
            else if (cp >= 0xdc00 && cp <= 0xdfff)
               break;
            if (decoded)
            {
               if (key_reserve(&buf, &cap, len, 4) != 0)
               {
                  free(buf);
                  return ECON_JSON_NO_MEMORY;
               }
               if (append_utf8(buf, cap, &len, cp) != 0)
                  break;
            }
         }
         else
         {
            uint8_t v;
            switch (e)
            {
            case '"':
               v = '"';
               break;
            case '\\':
               v = '\\';
               break;
            case '/':
               v = '/';
               break;
            case 'b':
               v = '\b';
               break;
            case 'f':
               v = '\f';
               break;
            case 'n':
               v = '\n';
               break;
            case 'r':
               v = '\r';
               break;
            case 't':
               v = '\t';
               break;
            default:
               goto invalid;
            }
            if (decoded)
            {
               if (key_reserve(&buf, &cap, len, 1) != 0)
               {
                  free(buf);
                  return ECON_JSON_NO_MEMORY;
               }
               buf[len++] = v;
            }
         }
      }
      else if (c < 0x80)
      {
         if (decoded)
         {
            if (key_reserve(&buf, &cap, len, 1) != 0)
            {
               free(buf);
               return ECON_JSON_NO_MEMORY;
            }
            buf[len++] = c;
         }
      }
      else
      {
         size_t used = 0;
         p->p--;
         if (utf8_sequence(p->s + p->p, p->n - p->p, &used) != 0)
         {
            free(buf);
            return ECON_JSON_INVALID_UTF8;
         }
         if (decoded)
         {
            if (key_reserve(&buf, &cap, len, used) != 0)
            {
               free(buf);
               return ECON_JSON_NO_MEMORY;
            }
            memcpy(buf + len, p->s + p->p, used);
            len += used;
         }
         p->p += used;
      }
   }
invalid:
   free(buf);
   return ECON_JSON_INVALID_SYNTAX;
}

static econ_json_result_t parse_value(parser_t *p, unsigned depth);

static econ_json_result_t parse_number(parser_t *p)
{
   size_t i = p->p;
   if (i < p->n && p->s[i] == '-')
      i++;
   if (i >= p->n)
      return ECON_JSON_INVALID_SYNTAX;
   if (p->s[i] == '0')
      i++;
   else if (p->s[i] >= '1' && p->s[i] <= '9')
      while (i < p->n && p->s[i] >= '0' && p->s[i] <= '9')
         i++;
   else
      return ECON_JSON_INVALID_SYNTAX;
   if (i < p->n && p->s[i] == '.')
   {
      if (++i >= p->n || p->s[i] < '0' || p->s[i] > '9')
         return ECON_JSON_INVALID_SYNTAX;
      while (i < p->n && p->s[i] >= '0' && p->s[i] <= '9')
         i++;
   }
   if (i < p->n && (p->s[i] == 'e' || p->s[i] == 'E'))
   {
      i++;
      if (i < p->n && (p->s[i] == '+' || p->s[i] == '-'))
         i++;
      if (i >= p->n || p->s[i] < '0' || p->s[i] > '9')
         return ECON_JSON_INVALID_SYNTAX;
      while (i < p->n && p->s[i] >= '0' && p->s[i] <= '9')
         i++;
   }
   p->p = i;
   return ECON_JSON_OK;
}

static econ_json_result_t parse_array(parser_t *p, unsigned depth)
{
   p->p++;
   skip_ws(p);
   if (p->p < p->n && p->s[p->p] == ']')
   {
      p->p++;
      return ECON_JSON_OK;
   }
   for (;;)
   {
      econ_json_result_t r = parse_value(p, depth + 1);
      if (r != ECON_JSON_OK)
         return r;
      skip_ws(p);
      if (p->p < p->n && p->s[p->p] == ']')
      {
         p->p++;
         return ECON_JSON_OK;
      }
      if (p->p >= p->n || p->s[p->p++] != ',')
         return ECON_JSON_INVALID_SYNTAX;
      skip_ws(p);
   }
}

static void free_keys(json_key_t *keys, size_t count)
{
   for (size_t i = 0; i < count; i++)
      free(keys[i].s);
   free(keys);
}

static int key_compare(const void *va, const void *vb)
{
   const json_key_t *a = va, *b = vb;
   size_t common = a->n < b->n ? a->n : b->n;
   int cmp = common ? memcmp(a->s, b->s, common) : 0;
   if (cmp)
      return cmp;
   return a->n < b->n ? -1 : a->n > b->n ? 1 : 0;
}

static int keys_unique(json_key_t *keys, size_t count)
{
   if (count < 2)
      return 1;
   qsort(keys, count, sizeof(*keys), key_compare);
   for (size_t i = 1; i < count; i++)
      if (keys[i - 1].n == keys[i].n &&
          (!keys[i].n || memcmp(keys[i - 1].s, keys[i].s, keys[i].n) == 0))
         return 0;
   return 1;
}

static econ_json_result_t parse_object(parser_t *p, unsigned depth)
{
   json_key_t *keys = NULL;
   size_t count = 0, cap = 0;
   p->p++;
   skip_ws(p);
   if (p->p < p->n && p->s[p->p] == '}')
   {
      p->p++;
      return ECON_JSON_OK;
   }
   for (;;)
   {
      if (count == ECON_JSON_MAX_OBJECT_MEMBERS)
      {
         free_keys(keys, count);
         return ECON_JSON_TOO_LARGE;
      }
      json_key_t key = {0};
      econ_json_result_t r = parse_string(p, &key);
      if (r != ECON_JSON_OK)
      {
         free_keys(keys, count);
         return r;
      }
      if (count == cap)
      {
         size_t next = cap ? cap * 2 : 8;
         json_key_t *grown = realloc(keys, next * sizeof(*keys));
         if (!grown)
         {
            free(key.s);
            free_keys(keys, count);
            return ECON_JSON_NO_MEMORY;
         }
         keys = grown;
         cap = next;
      }
      keys[count++] = key;
      skip_ws(p);
      if (p->p >= p->n || p->s[p->p++] != ':')
      {
         free_keys(keys, count);
         return ECON_JSON_INVALID_SYNTAX;
      }
      r = parse_value(p, depth + 1);
      if (r != ECON_JSON_OK)
      {
         free_keys(keys, count);
         return r;
      }
      skip_ws(p);
      if (p->p < p->n && p->s[p->p] == '}')
      {
         p->p++;
         int unique = keys_unique(keys, count);
         free_keys(keys, count);
         return unique ? ECON_JSON_OK : ECON_JSON_DUPLICATE_KEY;
      }
      if (p->p >= p->n || p->s[p->p++] != ',')
      {
         free_keys(keys, count);
         return ECON_JSON_INVALID_SYNTAX;
      }
      skip_ws(p);
   }
}

static econ_json_result_t parse_value(parser_t *p, unsigned depth)
{
   if (depth > ECON_JSON_MAX_DEPTH)
      return ECON_JSON_TOO_DEEP;
   skip_ws(p);
   if (p->p >= p->n)
      return ECON_JSON_INVALID_SYNTAX;
   uint8_t c = p->s[p->p];
   if (c == '"')
      return parse_string(p, NULL);
   if (c == '{')
      return parse_object(p, depth);
   if (c == '[')
      return parse_array(p, depth);
   if (c == '-' || (c >= '0' && c <= '9'))
      return parse_number(p);
   static const char *lit[] = {"true", "false", "null"};
   for (size_t i = 0; i < 3; i++)
   {
      size_t n = strlen(lit[i]);
      if (p->n - p->p >= n && memcmp(p->s + p->p, lit[i], n) == 0)
      {
         p->p += n;
         return ECON_JSON_OK;
      }
   }
   return ECON_JSON_INVALID_SYNTAX;
}

econ_json_result_t econ_json_compact(const void *input, size_t input_len, uint8_t **output,
                                     size_t *output_len)
{
   if (!output || !output_len || (!input && input_len))
      return ECON_JSON_INVALID_ARGUMENT;
   *output = NULL;
   *output_len = 0;
   if (!input_len || input_len > ECON_JSON_MAX_INPUT)
      return input_len > ECON_JSON_MAX_INPUT ? ECON_JSON_TOO_LARGE : ECON_JSON_INVALID_SYNTAX;
   parser_t p = {.s = input, .n = input_len, .p = 0};
   econ_json_result_t r = parse_value(&p, 0);
   if (r != ECON_JSON_OK)
      return r;
   skip_ws(&p);
   if (p.p != p.n)
      return ECON_JSON_INVALID_SYNTAX;

   uint8_t *dst = malloc(input_len + 1);
   if (!dst)
      return ECON_JSON_NO_MEMORY;
   size_t n = 0;
   int in_string = 0, escaped = 0;
   for (size_t i = 0; i < input_len; i++)
   {
      uint8_t c = ((const uint8_t *)input)[i];
      if (!in_string && ws(c))
         continue;
      dst[n++] = c;
      if (in_string)
      {
         if (escaped)
            escaped = 0;
         else if (c == '\\')
            escaped = 1;
         else if (c == '"')
            in_string = 0;
      }
      else if (c == '"')
         in_string = 1;
   }
   if (n >= input_len)
   {
      free(dst);
      return ECON_JSON_NOT_SHORTER;
   }
   dst[n] = 0;
   *output = dst;
   *output_len = n;
   return ECON_JSON_OK;
}

const char *econ_json_result_str(econ_json_result_t r)
{
   switch (r)
   {
   case ECON_JSON_OK:
      return "ok";
   case ECON_JSON_INVALID_ARGUMENT:
      return "invalid_argument";
   case ECON_JSON_TOO_LARGE:
      return "too_large";
   case ECON_JSON_TOO_DEEP:
      return "too_deep";
   case ECON_JSON_INVALID_UTF8:
      return "invalid_utf8";
   case ECON_JSON_INVALID_SYNTAX:
      return "invalid_syntax";
   case ECON_JSON_DUPLICATE_KEY:
      return "duplicate_key";
   case ECON_JSON_NOT_SHORTER:
      return "not_shorter";
   case ECON_JSON_NO_MEMORY:
      return "no_memory";
   }
   return "unknown";
}
