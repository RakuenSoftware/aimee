/* Open the standalone DB2 owner before it attaches to the event bus. */
#include "c/db2.h"

#include <stdio.h>
#include <stdlib.h>

int aimee_db2_module_init(void)
{
   const char *url = getenv("AIMEE_DB2_URL");
   if (!url || !url[0])
   {
      fprintf(stderr, "db2: AIMEE_DB2_URL is unset; refusing to serve\n");
      return -1;
   }
   if (db2_init(url) != 0)
   {
      /* A DSN can contain a password. Never echo it or a libpq diagnostic. */
      fprintf(stderr, "db2: database initialization failed; refusing to serve\n");
      return -1;
   }
   return 0;
}
