/* Open the standalone DB2 owner before it attaches to the event bus. */
#include "c/db2.h"
#include "config_embedder_dims.h"
#include "aimee.h"

#include <stdio.h>
#include <stdlib.h>

static void configure_embedding_dimension(void)
{
   int dimension = 0;
   int pinned = 0;
   const char *value = getenv("EMBEDDER_DIMS");
   if (value && value[0])
   {
      char *end = NULL;
      long parsed = strtol(value, &end, 10);
      if (end && *end == '\0' && parsed >= 1 && parsed <= EMBED_MAX_DIM)
      {
         dimension = (int)parsed;
         pinned = 1;
      }
      else
      {
         fprintf(stderr, "db2: EMBEDDER_DIMS must be 1..%d; using the declared default\n",
                 EMBED_MAX_DIM);
      }
   }
   db2_set_embedding_dim_default(CONFIG_EMBEDDER_DIMS_DEFAULT);
   db2_set_embedding_dim(dimension);
   db2_set_embedding_dim_pinned(pinned);
}

int aimee_db2_module_init(void)
{
   const char *url = getenv("AIMEE_DB2_URL");
   if (!url || !url[0])
   {
      fprintf(stderr, "db2: AIMEE_DB2_URL is unset; refusing to serve\n");
      return -1;
   }
   configure_embedding_dimension();
   if (db2_init(url) != 0)
   {
      /* A DSN can contain a password. Never echo it or a libpq diagnostic. */
      fprintf(stderr, "db2: database initialization failed; refusing to serve\n");
      return -1;
   }
   return 0;
}
