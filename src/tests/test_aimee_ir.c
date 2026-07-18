/* test_aimee_ir.c -- Slice 0: the canonical IR structs, the shape-agnostic
 * last-user-text extractor (the KB fix), stop-reason round-trip, and clean free. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee_ir.h"
#include "cJSON.h"

static char *dup(const char *s)
{
   return s ? strdup(s) : NULL;
}

/* helper: a heap message with `n` text/other blocks */
static aimee_block_t *mk_blocks(int n)
{
   return calloc((size_t)n, sizeof(aimee_block_t));
}

int main(void)
{
   printf("aimee-ir: ");

   /* --- last_user_text: last user message, concat TEXT blocks, drop non-text --- */
   aimee_request_t r;
   memset(&r, 0, sizeof r);
   r.n_messages = 3;
   r.messages = calloc(3, sizeof(aimee_message_t));

   r.messages[0].role = dup("user");
   r.messages[0].n_blocks = 1;
   r.messages[0].blocks = mk_blocks(1);
   r.messages[0].blocks[0].type = AIMEE_BLK_TEXT;
   r.messages[0].blocks[0].text = dup("first user turn");

   r.messages[1].role = dup("assistant");
   r.messages[1].n_blocks = 1;
   r.messages[1].blocks = mk_blocks(1);
   r.messages[1].blocks[0].type = AIMEE_BLK_TEXT;
   r.messages[1].blocks[0].text = dup("assistant reply");

   /* last user: two text blocks + a tool_result block that must NOT be concatenated
    * (but must not cause the message to be skipped) */
   r.messages[2].role = dup("user");
   r.messages[2].n_blocks = 3;
   r.messages[2].blocks = mk_blocks(3);
   r.messages[2].blocks[0].type = AIMEE_BLK_TEXT;
   r.messages[2].blocks[0].text = dup("fix ");
   r.messages[2].blocks[1].type = AIMEE_BLK_TOOL_RESULT;
   r.messages[2].blocks[1].tool_id = dup("call_1");
   r.messages[2].blocks[1].tool_result =
       cJSON_CreateString("output that must not leak into the query");
   r.messages[2].blocks[2].type = AIMEE_BLK_TEXT;
   r.messages[2].blocks[2].text = dup("the login bug");

   char q[128];
   size_t got = aimee_ir_last_user_text(&r, q, sizeof q);
   assert(got == strlen("fix the login bug"));
   assert(strcmp(q, "fix the login bug") == 0); /* tool_result dropped, texts joined */

   /* truncation: tiny buffer never overflows and is NUL-terminated */
   char tiny[6];
   size_t t = aimee_ir_last_user_text(&r, tiny, sizeof tiny);
   assert(t == 5 && strcmp(tiny, "fix t") == 0);

   /* NULL/empty guards */
   assert(aimee_ir_last_user_text(&r, NULL, 0) == 0);
   assert(aimee_ir_last_user_text(NULL, q, sizeof q) == 0 && q[0] == '\0');

   aimee_request_free(&r);
   /* free zeroes the struct */
   assert(r.messages == NULL && r.n_messages == 0);

   /* --- no user message -> empty --- */
   aimee_request_t r2;
   memset(&r2, 0, sizeof r2);
   r2.n_messages = 1;
   r2.messages = calloc(1, sizeof(aimee_message_t));
   r2.messages[0].role = dup("assistant");
   assert(aimee_ir_last_user_text(&r2, q, sizeof q) == 0 && q[0] == '\0');
   aimee_request_free(&r2);

   /* --- stop_reason canonical round-trip --- */
   aimee_stop_reason_t all[] = {AIMEE_STOP_END_TURN,       AIMEE_STOP_MAX_TOKENS,
                                AIMEE_STOP_TOOL_USE,       AIMEE_STOP_STOP_SEQUENCE,
                                AIMEE_STOP_CONTENT_FILTER, AIMEE_STOP_ERROR,
                                AIMEE_STOP_UNKNOWN};
   for (size_t i = 0; i < sizeof all / sizeof all[0]; i++)
      assert(aimee_stop_reason_parse(aimee_stop_reason_name(all[i])) == all[i]);
   assert(aimee_stop_reason_parse("bogus") == AIMEE_STOP_UNKNOWN);
   assert(aimee_stop_reason_parse(NULL) == AIMEE_STOP_UNKNOWN);

   /* --- response free with content blocks + sidecars (no leak / no crash) --- */
   aimee_response_t resp;
   memset(&resp, 0, sizeof resp);
   resp.id = dup("msg_1");
   resp.model = dup("codex");
   resp.role = dup("assistant");
   resp.stop_reason = AIMEE_STOP_TOOL_USE;
   resp.raw_stop_reason = dup("tool_calls");
   resp.n_content = 2;
   resp.content = mk_blocks(2);
   resp.content[0].type = AIMEE_BLK_TEXT;
   resp.content[0].text = dup("let me call a tool");
   resp.content[1].type = AIMEE_BLK_TOOL_USE;
   resp.content[1].tool_id = dup("call_9");
   resp.content[1].tool_name = dup("Read");
   resp.content[1].tool_input = cJSON_Parse("{\"path\":\"foo.c\"}");
   resp.raw = cJSON_CreateObject();
   aimee_response_free(&resp);
   assert(resp.content == NULL && resp.id == NULL);

   /* --- semantic equality: same content, different provenance -> equal --- */
   /* Build two 1-message requests with identical semantics but different model +
    * frontend + raw sidecar (as an Anthropic vs OpenAI parse would produce). */
   aimee_request_t a, b;
   for (int pass = 0; pass < 2; pass++)
   {
      aimee_request_t *x = pass ? &b : &a;
      memset(x, 0, sizeof *x);
      x->model = dup(pass ? "gpt-4o" : "claude-3-5-sonnet"); /* differs: provenance */
      x->frontend = pass ? AIMEE_WIRE_OPENAI_CHAT : AIMEE_WIRE_ANTHROPIC;
      x->raw = cJSON_CreateObject(); /* differs: provenance */
      x->n_messages = 1;
      x->messages = calloc(1, sizeof(aimee_message_t));
      x->messages[0].role = dup("user");
      x->messages[0].n_blocks = 1;
      x->messages[0].blocks = mk_blocks(1);
      x->messages[0].blocks[0].type = AIMEE_BLK_TEXT;
      x->messages[0].blocks[0].text = dup("summarize the repo");
   }
   assert(aimee_ir_request_equal(&a, &b)); /* provenance ignored -> equal */

   /* a divergent content byte -> not equal */
   free(b.messages[0].blocks[0].text);
   b.messages[0].blocks[0].text = dup("summarize the REPO");
   assert(!aimee_ir_request_equal(&a, &b));

   /* cache_control is semantic -> a difference makes them unequal */
   free(b.messages[0].blocks[0].text);
   b.messages[0].blocks[0].text = dup("summarize the repo");
   assert(aimee_ir_request_equal(&a, &b));
   b.messages[0].blocks[0].cache_control = dup("ephemeral");
   assert(!aimee_ir_request_equal(&a, &b));

   assert(aimee_ir_request_equal(NULL, NULL) == 1);
   assert(aimee_ir_request_equal(&a, NULL) == 0);
   aimee_request_free(&a);
   aimee_request_free(&b);

   /* --- response accessors: the delegate path's replacement for
    *     parsed_response_t.content / .is_tool_call ---
    *
    * The type-strictness is the whole point. THINKING must never be mistaken for
    * the answer: the legacy path had to regex <think> out of a flat string, and
    * got it wrong in several hand-rolled copies. Here reasoning is simply a
    * different block type, so "the answer" and "the reasoning" cannot be
    * confused by construction. */
   {
      aimee_response_t rsp;
      memset(&rsp, 0, sizeof rsp);
      rsp.n_content = 4;
      rsp.content = mk_blocks(4);
      rsp.content[0].type = AIMEE_BLK_THINKING;
      rsp.content[0].text = dup("weighing it up");
      rsp.content[1].type = AIMEE_BLK_TEXT;
      rsp.content[2].type = AIMEE_BLK_TEXT;
      rsp.content[1].text = dup("the ");
      rsp.content[2].text = dup("answer");
      rsp.content[3].type = AIMEE_BLK_TOOL_USE;
      rsp.content[3].tool_name = dup("grep");

      char buf[64];
      /* TEXT blocks concatenate in order; THINKING and TOOL_USE contribute nothing. */
      assert(aimee_ir_response_text(&rsp, buf, sizeof buf) == 10);
      assert(strcmp(buf, "the answer") == 0);
      /* reasoning is handed back, not destroyed */
      assert(aimee_ir_response_reasoning(&rsp, buf, sizeof buf) == 14);
      assert(strcmp(buf, "weighing it up") == 0);
      /* a TOOL_USE block anywhere means "it asked to call something" */
      assert(aimee_ir_response_has_tool_use(&rsp) == 1);
      aimee_response_free(&rsp);
   }

   /* A tool-only response says nothing: empty text, not the reasoning, not the
    * tool name. The legacy shape expressed this as content=NULL + is_tool_call=1. */
   {
      aimee_response_t rsp;
      memset(&rsp, 0, sizeof rsp);
      rsp.n_content = 2;
      rsp.content = mk_blocks(2);
      rsp.content[0].type = AIMEE_BLK_THINKING;
      rsp.content[0].text = dup("hmm");
      rsp.content[1].type = AIMEE_BLK_TOOL_USE;
      rsp.content[1].tool_name = dup("read");
      char buf[32];
      assert(aimee_ir_response_text(&rsp, buf, sizeof buf) == 0);
      assert(buf[0] == '\0');
      assert(aimee_ir_response_has_tool_use(&rsp) == 1);
      aimee_response_free(&rsp);
   }

   /* No tool blocks => has_tool_use is 0; and truncation must NUL-terminate
    * rather than overrun (buf smaller than the text). */
   {
      aimee_response_t rsp;
      memset(&rsp, 0, sizeof rsp);
      rsp.n_content = 1;
      rsp.content = mk_blocks(1);
      rsp.content[0].type = AIMEE_BLK_TEXT;
      rsp.content[0].text = dup("0123456789");
      char small[5];
      assert(aimee_ir_response_text(&rsp, small, sizeof small) == 4);
      assert(strcmp(small, "0123") == 0); /* truncated, terminated */
      assert(aimee_ir_response_has_tool_use(&rsp) == 0);
      aimee_response_free(&rsp);
   }

   /* NULL-safety, mirroring aimee_ir_last_user_text's contract. */
   {
      char buf[8] = "x";
      assert(aimee_ir_response_text(NULL, buf, sizeof buf) == 0);
      assert(buf[0] == '\0');
      assert(aimee_ir_response_reasoning(NULL, buf, sizeof buf) == 0);
      assert(aimee_ir_response_has_tool_use(NULL) == 0);
   }
   printf("response-accessors ok; ");

   /* aimee_ir_response_from_text: the bridge the tmux/CLI TUI handler uses to
    * fold flat scraped text into the unified message IR. */
   {
      aimee_response_t r;
      assert(aimee_ir_response_from_text(&r, "line one\nline two", "claude") == 0);
      assert(r.role && strcmp(r.role, "assistant") == 0);
      assert(r.n_content == 1 && r.content[0].type == AIMEE_BLK_TEXT);
      assert(r.stop_reason == AIMEE_STOP_END_TURN);
      assert(r.raw_stop_reason && strcmp(r.raw_stop_reason, "end_turn") == 0);
      assert(r.model && strcmp(r.model, "claude") == 0);
      char buf[64];
      /* Round-trips through the standard accessor -> a multi-line answer survives
       * verbatim, so the handler's projection is lossless. */
      assert(aimee_ir_response_text(&r, buf, sizeof buf) == (size_t)strlen("line one\nline two"));
      assert(strcmp(buf, "line one\nline two") == 0);
      assert(aimee_ir_response_has_tool_use(&r) == 0);
      aimee_response_free(&r);

      /* NULL/empty text is tolerated (empty TEXT block), model may be NULL. */
      assert(aimee_ir_response_from_text(&r, NULL, NULL) == 0);
      assert(r.n_content == 1 && r.content[0].text && r.content[0].text[0] == '\0');
      assert(r.model == NULL);
      aimee_response_free(&r);

      assert(aimee_ir_response_from_text(NULL, "x", "y") == -1);
   }
   printf("response-from-text ok; ");

   printf("ok\n");
   return 0;
}
