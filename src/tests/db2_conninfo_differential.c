/* Differential oracle: for any conninfo, does our bounded output preserve every
 * option the CALLER explicitly set, and add the ones they didn't?
 * "Explicitly set" is decided by libpq itself: parse the input, parse an empty
 * conninfo for the defaults, and treat a difference as caller intent. */
#include <libpq-fe.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int db2_pg_conninfo_with_bounds(const char *c, char *o, size_t n);

static const char *KEYS[] = {"connect_timeout", "keepalives", "keepalives_idle",
                             "keepalives_interval", "keepalives_count"};
static char *val_of(PQconninfoOption *o, const char *k)
{
   for (; o->keyword; o++)
      if (!strcmp(o->keyword, k))
         return o->val;
   return NULL;
}
static int seq(const char *a, const char *b)
{
   if (!a && !b)
      return 1;
   if (!a || !b)
      return 0;
   return !strcmp(a, b);
}

static int fails;
static void check(const char *in)
{
   char out[4096];
   if (db2_pg_conninfo_with_bounds(in, out, sizeof out) != 0)
      return; /* refused: fine */
   char *e1 = NULL, *e2 = NULL, *e3 = NULL;
   PQconninfoOption *base = PQconninfoParse(in, &e1);
   PQconninfoOption *dflt = PQconninfoParse("", &e2);
   PQconninfoOption *res = PQconninfoParse(out, &e3);
   if (!base)
   { /* input libpq rejects; only require we didn't make it parse */
      goto done;
   }
   if (!res)
   {
      printf("BROKE PARSE\n  in : %s\n  out: %s\n  %s", in, out, e3 ? e3 : "\n");
      fails++;
      goto done;
   }

   int caller_keepalives = !seq(val_of(base, "keepalives"), val_of(dflt, "keepalives"));
   for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
   {
      const char *k = KEYS[i];
      char *vb = val_of(base, k), *vd = val_of(dflt, k), *vr = val_of(res, k);
      int caller_set = !seq(vb, vd);
      if (caller_set && !seq(vr, vb))
      {
         printf("CALLER OVERRIDDEN (%s: wanted '%s', got '%s')\n  in : %s\n  out: %s\n", k,
                vb ? vb : "(null)", vr ? vr : "(null)", in, out);
         fails++;
      }
      if (!caller_set && (!vr || !*vr))
      {
         printf("BOUND MISSING (%s)\n  in : %s\n  out: %s\n", k, in, out);
         fails++;
      }
   }
done:
   if (base)
      PQconninfoFree(base);
   if (dflt)
      PQconninfoFree(dflt);
   if (res)
      PQconninfoFree(res);
   if (e1)
      PQfreemem(e1);
   if (e2)
      PQfreemem(e2);
   if (e3)
      PQfreemem(e3);
}

int main(void)
{
   const char *hosts[] = {"host=db",
                          "host=db\nport=5432",
                          "host=db\tport=5432",
                          "host=db  port=5432",
                          "  host=db",
                          "host='d b'",
                          "postgresql://h/db",
                          "postgres://h:5432/db",
                          "postgresql://u:p@h/db?sslmode=require"};
   const char *opts[] = {"",
                         "keepalives=0",
                         "keepalives=1",
                         "keepalives_idle=60",
                         "connect_timeout=3",
                         "keepalives_count=9",
                         "keepalives_interval=7",
                         "connect_timeout=3 keepalives=0"};
   char buf[1024];
   for (size_t h = 0; h < sizeof hosts / sizeof hosts[0]; h++)
      for (size_t o = 0; o < sizeof opts / sizeof opts[0]; o++)
      {
         if (!strncmp(hosts[h], "postgres", 8))
         {
            if (!*opts[o])
            {
               check(hosts[h]);
               continue;
            }
            char enc[256];
            snprintf(enc, sizeof enc, "%s", opts[o]);
            for (char *q = enc; *q; q++)
               if (*q == ' ')
                  *q = '&';
            snprintf(buf, sizeof buf, "%s%c%s", hosts[h], strchr(hosts[h], '?') ? '&' : '?', enc);
         }
         else
         {
            snprintf(buf, sizeof buf, "%s%s%s", hosts[h], *opts[o] ? " " : "", opts[o]);
         }
         check(buf);
      }
   /* percent-encoded variants */
   check("postgresql://h/db?connect%5Ftimeout=3");
   check("postgresql://h/db?keepalives%5Fidle=60");
   check("postgresql://h/db?keep%61lives=0");
   /* quoted values holding option text */
   check("host=db dbname='t keepalives=0'");
   check("host=db password='p connect_timeout=1'");
   check("host=db dbname='a\\' keepalives=0'");

   /* adversarial: whitespace around '=', exotic whitespace, duplicates,
    * prefix-sharing keys, odd percent-encoding, empty/trailing separators */
   check("host=db connect_timeout = 3");
   check("host=db connect_timeout =3");
   check("host=db connect_timeout= 3");
   check("host=db\rkeepalives=0");
   check("host=db\vkeepalives=0");
   check("host=db\fkeepalives=0");
   check("host=db\n\n\tkeepalives=0");
   check("host=db keepalives=0 keepalives=1");
   check("host=db keepalives_idle=60 keepalives=0");
   check("host=db keepalives=0 keepalives_idle=60");
   check("host=db dbname=''");
   check("host=db dbname='' keepalives=0");
   check("   ");
   check("host=db ");
   check("host=db  ");
   check("postgresql://h/db?");
   check("postgresql://h/db?&");
   check("postgresql://h/db?keepalives=0&");
   check("postgresql://h/db?&keepalives=0");
   check("postgresql://h/db?keepalives=0&keepalives=1");
   check("postgresql://h/db?keepalives%3d0");
   check("postgresql://h/db?%6beepalives=0");
   check("postgresql://h/db?%6Beepalives=0");
   check("postgresql://h/db?connect%5ftimeout=3");
   check("postgresql://h/db?keepalives_idle%3D60");
   check("postgresql://u:p%40x@h/db?keepalives=0");
   check("postgresql://h/db?sslmode=require&keepalives=0");
   check("host=db options='-c statement_timeout=5s'");
   check("host=db application_name='keepalives=0'");
   check("host=db dbname='trailing backslash\\\\'");
   printf(fails ? "\n=== %d DISAGREEMENT(S) ===\n" : "\n=== no disagreements ===\n", fails);
   return fails ? 1 : 0;
}
