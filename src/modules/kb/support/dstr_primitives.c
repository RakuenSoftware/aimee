/* Descriptor-owned DB2 process support for the required dstr_t lifecycle. */
#include "db2_dstr.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define DSTR_INIT_CAP 64

void dstr_init(dstr_t *s)
{
   s->data = NULL;
   s->len = 0;
   s->cap = 0;
}

static void dstr_reserve(dstr_t *s, size_t additional)
{
   size_t needed = s->len + additional + 1;
   if (needed <= s->cap)
      return;

   size_t newcap = s->cap ? s->cap : DSTR_INIT_CAP;
   while (newcap < needed)
      newcap *= 2;

   char *p = realloc(s->data, newcap);
   if (!p)
      return;
   s->data = p;
   s->cap = newcap;
}

static void dstr_vappendf(dstr_t *s, const char *fmt, va_list ap)
{
   va_list ap2;
   va_copy(ap2, ap);

   size_t avail = s->cap > s->len ? s->cap - s->len : 0;
   int n = vsnprintf(s->data ? s->data + s->len : NULL, avail, fmt, ap);
   if (n < 0)
   {
      va_end(ap2);
      return;
   }

   if ((size_t)n < avail)
   {
      s->len += (size_t)n;
      va_end(ap2);
      return;
   }

   dstr_reserve(s, (size_t)n);
   avail = s->cap > s->len ? s->cap - s->len : 0;
   if ((size_t)n >= avail)
   {
      if (s->data)
         s->data[s->len] = '\0';
      va_end(ap2);
      return;
   }
   vsnprintf(s->data + s->len, avail, fmt, ap2);
   s->len += (size_t)n;
   va_end(ap2);
}

void dstr_appendf(dstr_t *s, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   dstr_vappendf(s, fmt, ap);
   va_end(ap);
}

char *dstr_steal(dstr_t *s)
{
   char *p = s->data;
   s->data = NULL;
   s->len = 0;
   s->cap = 0;
   return p;
}
