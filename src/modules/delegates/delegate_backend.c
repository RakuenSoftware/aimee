/* delegate_backend.c: see delegate_backend.h.
 *
 * Process-global registry. Populated at server start; lookups are
 * lock-free reads of a fixed-size array. */

#include <aimee/delegates/delegate_backend.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static delegate_backend_t *g_registry[DELEGATE_BACKEND_MAX];
static int g_count;

int delegate_backend_register(delegate_backend_t *b)
{
   if (!b || !b->name || !b->name[0])
      return -1;
   if (g_count >= DELEGATE_BACKEND_MAX)
      return -1;
   for (int i = 0; i < g_count; i++)
   {
      if (g_registry[i]->name && strcmp(g_registry[i]->name, b->name) == 0)
         return -1;
   }
   g_registry[g_count++] = b;
   return 0;
}

delegate_backend_t *delegate_backend_lookup(const char *name)
{
   if (!name || !name[0])
      return NULL;
   for (int i = 0; i < g_count; i++)
   {
      if (g_registry[i]->name && strcmp(g_registry[i]->name, name) == 0)
         return g_registry[i];
   }
   return NULL;
}

int delegate_backend_list(delegate_backend_t **out, int max)
{
   if (!out || max <= 0)
      return 0;
   int n = g_count < max ? g_count : max;
   for (int i = 0; i < n; i++)
      out[i] = g_registry[i];
   return n;
}

void delegate_backend_reset_for_test(void)
{
   for (int i = 0; i < g_count; i++)
      g_registry[i] = NULL;
   g_count = 0;
}

/* Append `s` to *buf at *off, doubling *cap as needed. JSON-escapes
 * a minimal set of characters (", \\, control chars to \uXXXX). */
static int json_append_str(char **buf, size_t *cap, size_t *off, const char *s)
{
   if (!s)
      s = "";
   for (const char *p = s; *p; p++)
   {
      /* Worst case per char is 6 bytes (\uXXXX); keep 8 of headroom. */
      if (*off + 8 >= *cap)
      {
         size_t new_cap = *cap * 2;
         char *nb = realloc(*buf, new_cap);
         if (!nb)
            return -1;
         *buf = nb;
         *cap = new_cap;
      }
      unsigned char c = (unsigned char)*p;
      if (c == '"' || c == '\\')
      {
         (*buf)[(*off)++] = '\\';
         (*buf)[(*off)++] = (char)c;
      }
      else if (c < 0x20)
      {
         *off += (size_t)snprintf(*buf + *off, *cap - *off, "\\u%04x", c);
      }
      else
      {
         (*buf)[(*off)++] = (char)c;
      }
   }
   return 0;
}

char *delegate_backend_list_json(void)
{
   size_t cap = 256;
   char *buf = malloc(cap);
   if (!buf)
      return NULL;
   size_t off = 0;
   buf[off++] = '[';

   for (int i = 0; i < g_count; i++)
   {
      delegate_backend_t *b = g_registry[i];
      if (!b)
         continue;
      /* Reserve room for the entry skeleton — string body grows via
       * json_append_str. */
      if (off + 64 >= cap)
      {
         size_t new_cap = cap * 2;
         char *nb = realloc(buf, new_cap);
         if (!nb)
         {
            free(buf);
            return NULL;
         }
         buf = nb;
         cap = new_cap;
      }
      if (i > 0)
         buf[off++] = ',';
      memcpy(buf + off, "{\"name\":\"", 9);
      off += 9;
      if (json_append_str(&buf, &cap, &off, b->name) != 0)
      {
         free(buf);
         return NULL;
      }
      memcpy(buf + off, "\",\"description\":\"", 17);
      off += 17;
      if (json_append_str(&buf, &cap, &off, b->description) != 0)
      {
         free(buf);
         return NULL;
      }
      memcpy(buf + off, "\"}", 2);
      off += 2;
   }

   if (off + 2 >= cap)
   {
      char *nb = realloc(buf, off + 2);
      if (!nb)
      {
         free(buf);
         return NULL;
      }
      buf = nb;
      cap = off + 2;
   }
   buf[off++] = ']';
   buf[off] = '\0';
   return buf;
}
