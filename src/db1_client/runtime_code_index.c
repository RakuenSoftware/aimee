#include "runtime_code_index.h"
#include "db1_module_api.h"
#include "transport.h"
#include <stdlib.h>

static db1_family_t family = {"db1.runtime",           "runtime", AIMEE_DB1_EVENT_RUNTIME,
                              AIMEE_DB1_STAGE_RUNTIME, 120000,    0};
char *db1_runtime_code_index(const char *request_json)
{
   if (!request_json)
      return NULL;
   const char *fields[] = {request_json};
   char *result = malloc(1048576u);
   if (!result)
      return NULL;
   char *const values[] = {result};
   const size_t caps[] = {1048576u};
   if (db1_call(&family, AIMEE_DB1_OP_RUNTIME_CODE_INDEX, fields, 1, values, caps, 1, NULL) !=
       AIMEE_DB1_STATUS_OK)
   {
      free(result);
      return NULL;
   }
   return result;
}
