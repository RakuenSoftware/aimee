/* model_catalog_release.c: giving back a provider's model list.
 *
 * The catalog comes back as one contiguous block with no per-row owned
 * pointers, so this is a single free. It reads no database and answers no
 * query, which is why it lives here rather than in the module: the block a
 * caller frees is the one the client allocated on this side, and a copy inside
 * the module would be freeing an allocation it never made.
 */
#include <stdlib.h>

#include "model_catalog.h"

void db1_model_catalog_free(provider_model_t *models, int n)
{
   (void)n; /* one contiguous block; no per-row owned pointers */
   free(models);
}
