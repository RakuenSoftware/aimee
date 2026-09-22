/* Precision/error contract of the remaining CSS bus transport. */
#include "aimee.h"
#include "cJSON.h"
#include "json_fluent.h"
#include "kb/kb_service_css.h"
#include <assert.h>

static int unavailable;
static const char *typed_receipt;
int aimee_module_commands_dispatch_internal(const char *method, const cJSON *args, cJSON **out)
{
   assert(!strcmp(method, "memory.runtime"));
   if (!strcmp(jo_cstr(args, "operation"), "css-conventions") ||
       !strcmp(jo_cstr(args, "operation"), "css-convention-sync"))
   {
      assert(!strcmp(jo_cstr(args, "project"), "css-project"));
      *out = cJSON_CreateObject();
      assert(cJSON_AddStringToObject(*out, "json", typed_receipt));
      return unavailable ? -1 : 1;
   }
   assert(0 && "unexpected operation");
   return -1;
}
int main(void)
{
   typed_receipt =
       "{\"status\":\"ok\",\"value\":\"full 界 convention\",\"count\":9007199254742002}";
   for (int sync = 0; sync <= 1; sync++)
   {
      cJSON *css = db2_kb_service_css_conventions_json("css-project", sync);
      char *text = cJSON_PrintUnformatted(css);
      assert(text && !strcmp(text, typed_receipt));
      free(text);
      cJSON_Delete(css);
   }
   unavailable = 1;
   assert(!db2_kb_service_css_conventions_json("css-project", 1));
   unavailable = 0;
   const char *bad_css[] = {"[]", "{} trailing", "null"};
   for (unsigned i = 0; i < sizeof(bad_css) / sizeof(bad_css[0]); i++)
   {
      typed_receipt = bad_css[i];
      assert(!db2_kb_service_css_conventions_json("css-project", 0));
   }
   puts("CSS transport: exact JSON and failures passed");
   return 0;
}
