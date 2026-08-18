#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int init_result;
static int init_calls;
static char received_url[256];
static int default_dimension;
static int configured_dimension;
static int dimension_pinned;

void db2_set_embedding_dim_default(int dimension)
{
   default_dimension = dimension;
}

void db2_set_embedding_dim(int dimension)
{
   configured_dimension = dimension;
}

void db2_set_embedding_dim_pinned(int pinned)
{
   dimension_pinned = pinned;
}

int db2_init(const char *url)
{
   init_calls++;
   snprintf(received_url, sizeof received_url, "%s", url ? url : "");
   return init_result;
}

int aimee_db2_module_init(void);

int main(void)
{
   unsetenv("AIMEE_DB2_URL");
   unsetenv("EMBEDDER_DIMS");
   assert(aimee_db2_module_init() == -1);
   assert(init_calls == 0);

   const char *url = "postgresql://db2:secret@database/aimee";
   assert(setenv("AIMEE_DB2_URL", url, 1) == 0);
   assert(aimee_db2_module_init() == 0);
   assert(init_calls == 1);
   assert(strcmp(received_url, url) == 0);
   assert(default_dimension == 384);
   assert(configured_dimension == 0);
   assert(dimension_pinned == 0);

   assert(setenv("EMBEDDER_DIMS", "1024", 1) == 0);
   assert(aimee_db2_module_init() == 0);
   assert(init_calls == 2);
   assert(default_dimension == 384);
   assert(configured_dimension == 1024);
   assert(dimension_pinned == 1);

   assert(setenv("EMBEDDER_DIMS", "not-a-number", 1) == 0);
   assert(aimee_db2_module_init() == 0);
   assert(init_calls == 3);
   assert(default_dimension == 384);
   assert(configured_dimension == 0);
   assert(dimension_pinned == 0);

   init_result = -1;
   assert(aimee_db2_module_init() == -1);
   assert(init_calls == 4);

   puts("test_db2_module_init: fail-closed DSN and embedding dimension initialization ok");
   return 0;
}
