/* test_delegate_handoff.c: structured delegate handoff validation tests */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cmd_agent_delegate_impl.h"

static void test_prompt_contract_helpers(void)
{
   char *prompt = delegate_handoff_append_contract("Implement packet.", "packet-alpha");
   assert(prompt != NULL);
   assert(strstr(prompt, "delegate_result_v1") != NULL);
   assert(strstr(prompt, "packet-alpha") != NULL);
   free(prompt);

   char *repair = delegate_handoff_repair_prompt("bad response", "missing schema");
   assert(repair != NULL);
   assert(strstr(repair, "Repair it now") != NULL);
   assert(strstr(repair, "missing schema") != NULL);
   assert(strstr(repair, "bad response") != NULL);
   free(repair);
   printf("  PASS: test_prompt_contract_helpers\n");
}

static void test_validation_json_fields(void)
{
   /* The subject is what add_validation_json EMITS, so the verdict is stated
    * rather than computed: judging a handoff is the delegates module rule now
    * and this binary hosts no bus. The rule is covered in
    * server-go/modules/delegates/handoff_test.go. */
   delegate_handoff_validation_t v;
   memset(&v, 0, sizeof(v));
   v.valid = 1;
   v.passed_tests = 1;
   v.changed_files_count = 1;
   snprintf(v.status, sizeof(v.status), "%s", "done");
   snprintf(v.raw_status, sizeof(v.raw_status), "%s", "done");

   cJSON *obj = cJSON_CreateObject();
   delegate_handoff_add_validation_json(obj, &v);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(obj, "handoff_valid")));
   assert(strcmp(cJSON_GetObjectItem(obj, "handoff_status")->valuestring, "done") == 0);
   assert(cJSON_GetObjectItem(obj, "handoff_passed_tests")->valueint == 1);
   assert(cJSON_GetObjectItem(obj, "handoff_changed_files")->valueint == 1);
   cJSON_Delete(obj);
   printf("  PASS: test_validation_json_fields\n");
}

int main(void)
{
   test_prompt_contract_helpers();
   test_validation_json_fields();
   printf("delegate_handoff: all tests passed\n");
   return 0;
}
