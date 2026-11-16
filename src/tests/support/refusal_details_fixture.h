#ifndef AIMEE_TEST_REFUSAL_DETAILS_FIXTURE_H
#define AIMEE_TEST_REFUSAL_DETAILS_FIXTURE_H
/* The same snapshot round-trip runs in the registered agent-result/cache suite. */
static void test_refusal_snapshot_codec(void)
{
   aimee_refusal_details_t details = {.category = "future_category", .explanation = NULL};
   aimee_response_t source = {.role = "assistant",
                              .stop_reason = AIMEE_STOP_CONTENT_FILTER,
                              .raw_stop_reason = "refusal",
                              .refusal = &details};
   cJSON *json = aimee_ir_response_json(&source);
   assert(json);
   aimee_response_t decoded = {0};
   assert(!aimee_ir_response_parse(json, &decoded));
   assert(decoded.refusal && decoded.refusal != &details);
   assert(!strcmp(decoded.refusal->category, details.category));
   assert(!decoded.refusal->explanation && !strcmp(decoded.raw_stop_reason, "refusal"));
   cJSON *snapshot = aimee_ir_response_json(&decoded);
   assert(snapshot && cJSON_Compare(json, snapshot, 1));
   cJSON_Delete(snapshot);
   cJSON *refusal = cJSON_GetObjectItemCaseSensitive(json, "refusal");
   cJSON_AddStringToObject(refusal, "fallback_credit_token", "not retained");
   aimee_refusal_details_t *before = decoded.refusal;
   assert(aimee_ir_response_parse(json, &decoded) < 0 && decoded.refusal == before);
   cJSON_DeleteItemFromObjectCaseSensitive(refusal, "fallback_credit_token");
   cJSON_ReplaceItemInObjectCaseSensitive(json, "refusal", cJSON_CreateNull());
   assert(!aimee_ir_response_parse(json, &decoded) && !decoded.refusal);
   cJSON_Delete(json);
   details.category = NULL;
   json = aimee_ir_response_json(&source);
   assert(json && !aimee_ir_response_parse(json, &decoded));
   assert(decoded.refusal && !decoded.refusal->category && !decoded.refusal->explanation);
   cJSON_Delete(json);
   source.refusal = NULL;
   json = aimee_ir_response_json(&source);
   assert(json && !cJSON_HasObjectItem(json, "refusal"));
   cJSON_Delete(json);
   aimee_response_free(&decoded);
}
#endif
