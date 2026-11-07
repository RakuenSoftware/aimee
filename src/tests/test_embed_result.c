/* Exercise both the message codec and the real KB batch consumer. */
#include "../kb/kb.c"
#include <assert.h>

static cJSON *embedding_reply;

int aimee_module_commands_dispatch_internal(const char *method, const cJSON *args, cJSON **result)
{
   assert(!strcmp(method, "memory.embed_text"));
   assert(!strcmp(jo_cstr(args, "operation"), "batch"));
   assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(args, "texts")) == 2);
   *result = cJSON_Duplicate(embedding_reply, 1);
   return *result ? 0 : -1;
}

static cJSON *batch(int width)
{
   cJSON *reply = cJSON_CreateObject();
   cJSON_AddNumberToObject(reply, "dim", width);
   cJSON *rows = cJSON_AddArrayToObject(reply, "vectors");
   for (int row = 0; row < 2; ++row)
   {
      cJSON *values = cJSON_CreateArray();
      cJSON_AddItemToArray(rows, values);
      for (int i = 0; i < width; ++i)
         cJSON_AddItemToArray(values, cJSON_CreateNumber((row + 1) * 100 + (i % 19) * 0.25));
   }
   return reply;
}

static void actual_batch_consumer(int width)
{
   embedding_reply = batch(width);
   text_chunk_t *chunks = calloc(2, sizeof(*chunks));
   assert(chunks);
   snprintf(chunks[0].content, sizeof(chunks[0].content), "first document");
   snprintf(chunks[1].content, sizeof(chunks[1].content), "second document");
   kb_build_file_ctx_t context = {.chunks = chunks, .effective_cmd = "http://embedder"};
   float *out = malloc(2 * EMBED_MAX_DIM * sizeof(*out));
   assert(out);
   for (int i = 0; i < 2 * EMBED_MAX_DIM; ++i)
      out[i] = -999;
   /* Row count is two, width is three or 4000, buffer stride is 4000. */
   assert(kb_embed_file_chunks(&context, 2, out) == width);
   for (int row = 0; row < 2; ++row)
      for (int i = 0; i < width; ++i)
         assert(out[row * EMBED_MAX_DIM + i] == (float)((row + 1) * 100 + (i % 19) * 0.25));
   if (width < EMBED_MAX_DIM)
      assert(out[EMBED_MAX_DIM + width] == -999);
   cJSON_AddBoolToObject(embedding_reply, "truncated", 1);
   float before = out[0];
   assert(!kb_embed_file_chunks(&context, 2, out) && out[0] == before);
   cJSON_Delete(embedding_reply);
   embedding_reply = NULL;
   free(out);
   free(chunks);
}

static void vector_messages(void)
{
   const char *invalid[] = {"{}",
                            "{\"dim\":2,\"vector\":[1]}",
                            "{\"dim\":1.5,\"vector\":[1]}",
                            "{\"dim\":2,\"vector\":[1,null]}",
                            "{\"dim\":2,\"vector\":[1,1e999]}",
                            "{\"dim\":2,\"vector\":[1,2],\"truncated\":true}",
                            "{\"dim\":2,\"vector\":[1,2],\"unauthorized\":true}",
                            "{\"dim\":2,\"vector\":[1,2],\"unavailable\":true}",
                            "{\"dim\":2,\"vector\":[1,2],\"error\":\"provider failure\"}",
                            "{\"dim\":2,\"vector\":[1,2],\"truncated\":\"false\"}"};
   for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
   {
      cJSON *reply = cJSON_Parse(invalid[i]);
      assert(reply);
      float out[] = {91, 92, 93};
      assert(!embed_reply_vector(reply, out, 3));
      assert(out[0] == 91 && out[1] == 92 && out[2] == 93);
      cJSON_Delete(reply);
   }
   cJSON *reply = cJSON_Parse("{\"dim\":2,\"vector\":[1.25,-2],\"truncated\":false}");
   float out[3] = {0};
   assert(embed_reply_vector(reply, out, 3) == 2 && out[0] == 1.25 && out[1] == -2);
   assert(!embed_reply_vector(reply, out, 1));
   cJSON_Delete(reply);
}

static void invalid_matrix_is_atomic(void)
{
   const char *messages[] = {
       "{\"dim\":3,\"vectors\":[[1,2,3],[4,5]]}", "{\"dim\":3,\"vectors\":[[1,2,3],[4,5,null]]}",
       "{\"dim\":3,\"vectors\":[[1,2,3]]}", "{\"dim\":6,\"vectors\":[[1,2,3],[4,5,6]]}"};
   for (size_t i = 0; i < sizeof(messages) / sizeof(messages[0]); ++i)
   {
      cJSON *reply = cJSON_Parse(messages[i]);
      assert(reply);
      float out[10], before[10];
      for (int j = 0; j < 10; ++j)
         before[j] = out[j] = (float)(j + 100);
      assert(!embed_reply_matrix(reply, out, 2, 5));
      assert(!memcmp(out, before, sizeof(out)));
      cJSON_Delete(reply);
   }
}

int main(void)
{
   vector_messages();
   invalid_matrix_is_atomic();
   actual_batch_consumer(3);
   actual_batch_consumer(EMBED_MAX_DIM);
   puts("embedding messages: actual KB width/stride, complete vectors and decline facts pass");
   return 0;
}
