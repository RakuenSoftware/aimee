/* memory_extract_patterns.c: pattern-first fact extraction (§6) + retraction
 * scan (§4). Pure logic, no DB. See memory_extract_patterns.h. P5. */
#include "headers/memory_extract_patterns.h"
#include "headers/rel_types.h" /* rel_type_normalize */

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int is_hex(char c)
{
   return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int all_digits(const char *s, int n)
{
   for (int i = 0; i < n; i++)
      if (!isdigit((unsigned char)s[i]))
         return 0;
   return n > 0;
}

/* An IPv4 dotted-quad: four 1-3 digit octets, each 0-255. */
static int is_ipv4(const char *t)
{
   int octets = 0;
   const char *p = t;
   while (*p)
   {
      int len = 0, val = 0;
      while (isdigit((unsigned char)p[len]))
      {
         val = val * 10 + (p[len] - '0');
         len++;
         if (len > 3)
            return 0;
      }
      if (len == 0 || val > 255)
         return 0;
      p += len;
      octets++;
      if (*p == '.')
         p++;
      else if (*p == '\0')
         break;
      else
         return 0;
   }
   return octets == 4;
}

/* A MAC address: exactly 6 two-hex-digit groups joined by a single consistent
 * ':' or '-' separator (aa:bb:cc:dd:ee:ff). */
static int is_mac(const char *t)
{
   size_t n = strlen(t);
   if (n != 17)
      return 0;
   char sep = t[2];
   if (sep != ':' && sep != '-')
      return 0;
   for (int g = 0; g < 6; g++)
   {
      const char *grp = t + g * 3;
      if (!is_hex(grp[0]) || !is_hex(grp[1]))
         return 0;
      if (g < 5 && grp[2] != sep)
         return 0;
   }
   return 1;
}

/* An IPv6 address (conservative): contains a "::" run, or exactly 8 groups of
 * 1-4 hex digits joined by ':'. Excludes the 6-group MAC shape. */
static int is_ipv6(const char *t)
{
   if (!strchr(t, ':'))
      return 0;
   for (const char *p = t; *p; p++)
      if (!is_hex(*p) && *p != ':')
         return 0;
   if (strstr(t, "::"))
      return 1; /* compressed form */
   /* count colon-separated groups of 1-4 hex digits */
   int groups = 0, len = 0;
   for (const char *p = t;; p++)
   {
      if (*p == ':' || *p == '\0')
      {
         if (len < 1 || len > 4)
            return 0;
         groups++;
         len = 0;
         if (*p == '\0')
            break;
      }
      else
      {
         len++;
      }
   }
   return groups == 8;
}

/* An email: one '@', non-empty local part, domain with an interior dot, and only
 * conservative characters throughout. */
static int is_email(const char *t)
{
   const char *at = strchr(t, '@');
   if (!at || at == t || strchr(at + 1, '@'))
      return 0;
   const char *dom = at + 1;
   const char *dot = strchr(dom, '.');
   if (!dot || dot == dom || dot[1] == '\0')
      return 0;
   for (const char *p = t; *p; p++)
   {
      char c = *p;
      if (c == '@' || c == '.' || c == '-' || c == '_' || c == '+' || isalnum((unsigned char)c))
         continue;
      return 0;
   }
   return 1;
}

/* An ISO calendar date YYYY-MM-DD with in-range month/day. */
static int is_iso_date(const char *t)
{
   if (strlen(t) != 10 || t[4] != '-' || t[7] != '-')
      return 0;
   if (!all_digits(t, 4) || !all_digits(t + 5, 2) || !all_digits(t + 8, 2))
      return 0;
   int mo = (t[5] - '0') * 10 + (t[6] - '0');
   int da = (t[8] - '0') * 10 + (t[9] - '0');
   return mo >= 1 && mo <= 12 && da >= 1 && da <= 31;
}

pattern_value_kind_t memory_pattern_classify_value(const char *token)
{
   if (!token || !token[0])
      return PAT_VAL_NONE;
   if (is_ipv4(token))
      return PAT_VAL_IPV4;
   if (is_mac(token))
      return PAT_VAL_MAC;
   if (is_ipv6(token))
      return PAT_VAL_IPV6;
   if (is_email(token))
      return PAT_VAL_EMAIL;
   if (is_iso_date(token))
      return PAT_VAL_DATE;
   return PAT_VAL_NONE;
}

memory_node_kind_t memory_pattern_value_node_kind(pattern_value_kind_t k)
{
   switch (k)
   {
   case PAT_VAL_IPV4:
   case PAT_VAL_IPV6:
      return NODE_IP;
   case PAT_VAL_MAC:
   case PAT_VAL_EMAIL:
   case PAT_VAL_DATE:
      return NODE_SCALAR;
   default:
      return NODE_OTHER;
   }
}

/* Case-insensitive substring search; returns index or -1. */
static int ci_find(const char *hay, const char *needle, int from)
{
   int hn = (int)strlen(hay), nn = (int)strlen(needle);
   if (nn == 0)
      return from;
   for (int i = from; i + nn <= hn; i++)
   {
      int k = 0;
      while (k < nn && tolower((unsigned char)hay[i + k]) == tolower((unsigned char)needle[k]))
         k++;
      if (k == nn)
         return i;
   }
   return -1;
}

int memory_pattern_is_retraction(const char *text)
{
   if (!text || !text[0])
      return 0;
   static const char *cues[] = {"forget",      "delete that", "delete the",   "that's wrong",
                                "thats wrong", "no longer",   "scratch that", "ignore that",
                                "never mind",  "nevermind",   "disregard",    "that is wrong"};
   for (size_t i = 0; i < sizeof(cues) / sizeof(cues[0]); i++)
      if (ci_find(text, cues[i], 0) >= 0)
         return 1;
   return 0;
}

/* Trim leading/trailing ASCII whitespace, copying at most cap-1 bytes. */
static void copy_trimmed(char *dst, size_t cap, const char *src, int len)
{
   int s = 0, e = len;
   while (s < e && isspace((unsigned char)src[s]))
      s++;
   while (e > s && isspace((unsigned char)src[e - 1]))
      e--;
   int n = e - s;
   if (n < 0)
      n = 0;
   if ((size_t)n >= cap)
      n = (int)cap - 1;
   memcpy(dst, src + s, (size_t)n);
   dst[n] = '\0';
}

/* True if position i in text begins a "my" word (boundary before, space after). */
static int is_my_word(const char *text, int i)
{
   if (tolower((unsigned char)text[i]) != 'm' || tolower((unsigned char)text[i + 1]) != 'y')
      return 0;
   if (i > 0 && (isalnum((unsigned char)text[i - 1]) || text[i - 1] == '_'))
      return 0; /* not a word start (e.g. "army") */
   return isspace((unsigned char)text[i + 2]);
}

int memory_extract_patterns(const char *text, pattern_triple_t *out, int max)
{
   if (!text || !out || max <= 0)
      return -1;
   int n = 0, len = (int)strlen(text);
   for (int i = 0; i < len && n < max; i++)
   {
      if (!is_my_word(text, i))
         continue;
      int attr_start = i + 2;
      while (attr_start < len && isspace((unsigned char)text[attr_start]))
         attr_start++;
      /* locate the " is " separator that ends the attribute. */
      int is_pos = ci_find(text, " is ", attr_start);
      if (is_pos < 0)
         continue;
      int val_start = is_pos + 4;
      /* value runs to a sentence terminator or end of text. A '.', '!' or '?'
       * ends the sentence only when followed by whitespace/end — so an interior
       * dot (e.g. "example.com" / "192.168.1.254") does not truncate the value. */
      int val_end = val_start;
      while (val_end < len)
      {
         char c = text[val_end];
         if (c == '\n')
            break;
         if ((c == '.' || c == '!' || c == '?') &&
             (text[val_end + 1] == '\0' || isspace((unsigned char)text[val_end + 1])))
            break;
         val_end++;
      }

      char attr[128], value[128];
      copy_trimmed(attr, sizeof(attr), text + attr_start, is_pos - attr_start);
      copy_trimmed(value, sizeof(value), text + val_start, val_end - val_start);
      if (!attr[0] || !value[0])
      {
         i = val_end;
         continue;
      }

      pattern_triple_t *t = &out[n];
      memset(t, 0, sizeof(*t));
      snprintf(t->subject, sizeof(t->subject), "user");
      rel_type_normalize(attr, t->rel_type, sizeof(t->rel_type));
      snprintf(t->object, sizeof(t->object), "%s", value);
      t->subject_kind = NODE_PERSON;
      t->object_kind = memory_pattern_value_node_kind(memory_pattern_classify_value(value));
      if (t->rel_type[0])
         n++;
      i = val_end; /* continue past this match */
   }
   return n;
}
