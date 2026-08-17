#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int init_result;
static int init_calls;
static char received_url[256];

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
   assert(aimee_db2_module_init() == -1);
   assert(init_calls == 0);

   const char *url = "postgresql://db2:secret@database/aimee";
   assert(setenv("AIMEE_DB2_URL", url, 1) == 0);
   assert(aimee_db2_module_init() == 0);
   assert(init_calls == 1);
   assert(strcmp(received_url, url) == 0);

   init_result = -1;
   assert(aimee_db2_module_init() == -1);
   assert(init_calls == 2);

   puts("test_db2_module_init: fail-closed DSN initialization ok");
   return 0;
}
