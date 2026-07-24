/* test_aimee_converse_stream.c -- Slice P6c-stream: AWS Bedrock ConverseStream
 * events -> IR deltas via bedrock_converse_stream_to_deltas. Pure, offline,
 * fixture-tested against AWS's documented ConverseStream event shapes. No DB.
 * Mirrors the openai_chunk_to_deltas test discipline. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <aimee/translation/aimee_ir_stream.h>
#include "cJSON.h"

/* Parse `json` and run one ConverseStream event through the parser. Returns the
 * count; deltas + the parsed payload are handed back so the caller can assert on
 * borrowed fields (payload must outlive the assertions). */
static int run(const char *event_type, const char *json, converse_stream_state_t *st,
               aimee_delta_t *out, int max, cJSON **payload_out)
{
   cJSON *payload = json ? cJSON_Parse(json) : NULL;
   if (json)
      assert(payload);
   int n = bedrock_converse_stream_to_deltas(event_type, payload, st, out, max);
   *payload_out = payload; /* caller frees after asserting on borrowed const char* */
   return n;
}

int main(void)
{
   printf("converse-stream: ");
   aimee_delta_t d[8];
   cJSON *pl = NULL;

   /* (a) messageStart -> 1 TURN_START */
   {
      converse_stream_state_t st;
      converse_stream_state_init(&st);
      int n = run("messageStart", "{\"role\":\"assistant\"}", &st, d, 8, &pl);
      assert(n == 1);
      assert(d[0].type == AIMEE_DELTA_TURN_START);
      cJSON_Delete(pl);
   }

   /* (b) contentBlockStart is tool-use only; text/reasoning open on first delta. */
   {
      converse_stream_state_t st;
      converse_stream_state_init(&st);
      int n = run("contentBlockStart",
                  "{\"contentBlockIndex\":0,\"start\":{\"toolUse\":{\"toolUseId\":\"t1\","
                  "\"name\":\"fn\"}}}",
                  &st, d, 8, &pl);
      assert(n == 1);
      assert(d[0].type == AIMEE_DELTA_BLOCK_START);
      assert(d[0].kind == AIMEE_BLK_TOOL_USE);
      assert(d[0].block_id == 0);
      assert(d[0].tool_id && strcmp(d[0].tool_id, "t1") == 0);
      assert(d[0].tool_name && strcmp(d[0].tool_name, "fn") == 0);
      cJSON_Delete(pl);

      n = run("contentBlockStart", "{\"contentBlockIndex\":1}", &st, d, 8, &pl);
      assert(n == -1);
      cJSON_Delete(pl);
   }

   /* (c) contentBlockDelta: text / toolUse.input / reasoningContent.text */
   {
      converse_stream_state_t st;
      converse_stream_state_init(&st);
      int n = run("contentBlockDelta", "{\"contentBlockIndex\":1,\"delta\":{\"text\":\"hi\"}}", &st,
                  d, 8, &pl);
      assert(n == 2 && d[0].type == AIMEE_DELTA_BLOCK_START);
      assert(d[0].kind == AIMEE_BLK_TEXT && d[0].block_id == 1);
      assert(d[1].type == AIMEE_DELTA_BLOCK_DELTA && d[1].kind == AIMEE_BLK_TEXT);
      assert(d[1].text_delta && strcmp(d[1].text_delta, "hi") == 0);
      cJSON_Delete(pl);

      n = run("contentBlockStart",
              "{\"contentBlockIndex\":0,\"start\":{\"toolUse\":{\"toolUseId\":\"t1\","
              "\"name\":\"fn\"}}}",
              &st, d, 8, &pl);
      assert(n == 1);
      cJSON_Delete(pl);
      n = run("contentBlockDelta",
              "{\"contentBlockIndex\":0,\"delta\":{\"toolUse\":{\"input\":\"{\\\"p\\\":\"}}}", &st,
              d, 8, &pl);
      assert(n == 1);
      assert(d[0].type == AIMEE_DELTA_BLOCK_DELTA);
      assert(d[0].kind == AIMEE_BLK_TOOL_USE);
      assert(d[0].block_id == 0);
      assert(d[0].tool_args_delta && strcmp(d[0].tool_args_delta, "{\"p\":") == 0);
      cJSON_Delete(pl);

      n = run("contentBlockDelta",
              "{\"contentBlockIndex\":2,\"delta\":{\"reasoningContent\":{\"text\":\"think\"}}}",
              &st, d, 8, &pl);
      assert(n == 2 && d[0].type == AIMEE_DELTA_BLOCK_START);
      assert(d[0].kind == AIMEE_BLK_THINKING && d[0].block_id == 2);
      assert(d[1].type == AIMEE_DELTA_BLOCK_DELTA);
      assert(d[1].text_delta && strcmp(d[1].text_delta, "think") == 0);
      cJSON_Delete(pl);

      converse_stream_state_t fresh;
      converse_stream_state_init(&fresh);
      n = run("contentBlockDelta", "{\"contentBlockIndex\":3,\"delta\":{\"text\":\"x\"}}", &fresh,
              d, 1, &pl);
      assert(n == -1 && !fresh.kind_set[3]);
      cJSON_Delete(pl);
      n = run("contentBlockDelta", "{\"contentBlockIndex\":3,\"delta\":{\"text\":\"x\"}}", &fresh,
              d, 2, &pl);
      assert(n == 2);
      cJSON_Delete(pl);
   }

   /* (d) contentBlockStop carries the tracked kind (seed a start first) */
   {
      converse_stream_state_t st;
      converse_stream_state_init(&st);
      int n = run("contentBlockStart",
                  "{\"contentBlockIndex\":0,\"start\":{\"toolUse\":{\"toolUseId\":\"t1\","
                  "\"name\":\"fn\"}}}",
                  &st, d, 8, &pl);
      assert(n == 1);
      cJSON_Delete(pl);
      n = run("contentBlockStop", "{\"contentBlockIndex\":0}", &st, d, 8, &pl);
      assert(n == 1);
      assert(d[0].type == AIMEE_DELTA_BLOCK_STOP);
      assert(d[0].block_id == 0);
      assert(d[0].kind == AIMEE_BLK_TOOL_USE);
      cJSON_Delete(pl);
   }

   /* (e/f) messageStop defers one terminal delta until metadata supplies usage. */
   {
      converse_stream_state_t st;
      converse_stream_state_init(&st);
      int n = run("messageStop", "{\"stopReason\":\"tool_use\"}", &st, d, 8, &pl);
      assert(n == 0 && st.message_stop_seen);
      cJSON_Delete(pl);
      n = run("metadata", "{\"usage\":{\"inputTokens\":12,\"outputTokens\":34}}", &st, d, 8, &pl);
      assert(n == 1 && d[0].type == AIMEE_DELTA_TURN_STOP);
      assert(d[0].stop_reason == AIMEE_STOP_TOOL_USE);
      assert(d[0].usage_in == 12 && d[0].usage_out == 34);
      cJSON_Delete(pl);
      n = run("metadata", "{\"usage\":{\"inputTokens\":1,\"outputTokens\":1}}", &st, d, 8, &pl);
      assert(n == -1);
      cJSON_Delete(pl);
   }

   /* metadata-before-stop and duplicate messageStop are malformed. */
   {
      converse_stream_state_t st;
      converse_stream_state_init(&st);
      int n =
          run("metadata", "{\"usage\":{\"inputTokens\":12,\"outputTokens\":34}}", &st, d, 8, &pl);
      assert(n == -1);
      cJSON_Delete(pl);
      n = run("messageStop", "{\"stopReason\":\"end_turn\"}", &st, d, 8, &pl);
      assert(n == 0);
      cJSON_Delete(pl);
      n = run("messageStop", "{\"stopReason\":\"end_turn\"}", &st, d, 8, &pl);
      assert(n == -1);
      cJSON_Delete(pl);
   }

   /* (g) unknown event_type -> 0 deltas */
   {
      converse_stream_state_t st;
      converse_stream_state_init(&st);
      int n = run("somethingNew", "{\"whatever\":1}", &st, d, 8, &pl);
      assert(n == 0);
      cJSON_Delete(pl);
   }

   /* (i) interleaved: three blocks with distinct kinds, then stop each -> BLOCK_STOP
    * carries the right kind (state correctness), one SHARED state. */
   {
      converse_stream_state_t st;
      converse_stream_state_init(&st);
      int n = run("contentBlockStart",
                  "{\"contentBlockIndex\":0,\"start\":{\"toolUse\":{\"toolUseId\":\"t\","
                  "\"name\":\"f\"}}}",
                  &st, d, 8, &pl);
      assert(n == 1 && d[0].kind == AIMEE_BLK_TOOL_USE);
      cJSON_Delete(pl);
      n = run("contentBlockDelta", "{\"contentBlockIndex\":1,\"delta\":{\"text\":\"x\"}}", &st, d,
              8, &pl);
      assert(n == 2 && d[0].kind == AIMEE_BLK_TEXT && d[1].kind == AIMEE_BLK_TEXT);
      cJSON_Delete(pl);
      /* block 2's kind established via a reasoning delta (no explicit start) */
      n = run("contentBlockDelta",
              "{\"contentBlockIndex\":2,\"delta\":{\"reasoningContent\":{\"text\":\"r\"}}}", &st, d,
              8, &pl);
      assert(n == 2 && d[0].kind == AIMEE_BLK_THINKING && d[1].kind == AIMEE_BLK_THINKING);
      cJSON_Delete(pl);

      n = run("contentBlockStop", "{\"contentBlockIndex\":0}", &st, d, 8, &pl);
      assert(n == 1 && d[0].kind == AIMEE_BLK_TOOL_USE && d[0].block_id == 0);
      cJSON_Delete(pl);
      n = run("contentBlockStop", "{\"contentBlockIndex\":1}", &st, d, 8, &pl);
      assert(n == 1 && d[0].kind == AIMEE_BLK_TEXT && d[0].block_id == 1);
      cJSON_Delete(pl);
      n = run("contentBlockStop", "{\"contentBlockIndex\":2}", &st, d, 8, &pl);
      assert(n == 1 && d[0].kind == AIMEE_BLK_THINKING && d[0].block_id == 2);
      cJSON_Delete(pl);
   }

   /* (j) exception event -> 1 ERROR delta with error_message */
   {
      converse_stream_state_t st;
      converse_stream_state_init(&st);
      int n = run("throttlingException", "{\"message\":\"slow down\"}", &st, d, 8, &pl);
      assert(n == 1 && d[0].type == AIMEE_DELTA_ERROR);
      assert(d[0].error_message && strcmp(d[0].error_message, "slow down") == 0);
      cJSON_Delete(pl);

      /* no message -> error_message falls back to the event_type */
      n = run("validationException", "{}", &st, d, 8, &pl);
      assert(n == 1 && d[0].type == AIMEE_DELTA_ERROR);
      assert(d[0].error_message && strcmp(d[0].error_message, "validationException") == 0);
      cJSON_Delete(pl);
   }

   /* (k) an unknown union member inside a known event is malformed; a
    * non-object delta is malformed too. */
   {
      converse_stream_state_t st;
      converse_stream_state_init(&st);
      int n = run("contentBlockDelta",
                  "{\"contentBlockIndex\":0,\"delta\":{\"citation\":{\"title\":\"x\"}}}", &st, d, 8,
                  &pl);
      assert(n == -1);
      cJSON_Delete(pl);

      n = run("contentBlockDelta", "{\"contentBlockIndex\":0,\"delta\":\"not-an-object\"}", &st, d,
              8, &pl);
      assert(n == -1);
      cJSON_Delete(pl);
   }

   /* (l) an out-of-range contentBlockIndex on a contentBlock* event -> -1 (no
    * unbounded block_id escapes to the consumer). */
   {
      converse_stream_state_t st;
      converse_stream_state_init(&st);
      int n = run("contentBlockStart", "{\"contentBlockIndex\":-1}", &st, d, 8, &pl);
      assert(n == -1);
      cJSON_Delete(pl);
      n = run("contentBlockStop", "{\"contentBlockIndex\":99999}", &st, d, 8, &pl);
      assert(n == -1);
      cJSON_Delete(pl);
      n = run("contentBlockDelta", "{\"contentBlockIndex\":100000,\"delta\":{\"text\":\"x\"}}", &st,
              d, 8, &pl);
      assert(n == -1);
      cJSON_Delete(pl);
   }

   printf("ok\n");
   return 0;
}
