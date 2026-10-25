/* Opt-in, transport-free production codec diagnostic. Bodies stay in private
 * files; stdout reports structure only. This does not simulate source custody,
 * credential admission, SDK launch, or native cache ownership. */
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
static int offline_model_request_diagnostic(int argc, char **argv)
{
   if (argc != 5)
      return 2;
   aimee_wire_t wire = !strcmp(argv[3], "anthropic")   ? AIMEE_WIRE_ANTHROPIC
                       : !strcmp(argv[3], "openai")    ? AIMEE_WIRE_OPENAI_CHAT
                       : !strcmp(argv[3], "responses") ? AIMEE_WIRE_RESPONSES
                                                       : AIMEE_WIRE_UNKNOWN;
   if (!wire)
      return 2;
   FILE *input = fopen(argv[2], "rb");
   if (!input)
      return 2;
   if (fseek(input, 0, SEEK_END))
   {
      fclose(input);
      return 2;
   }
   long length = ftell(input);
   if (length < 0 || length > 16 * 1024 * 1024 || fseek(input, 0, SEEK_SET))
   {
      fclose(input);
      return 2;
   }
   char *bytes = calloc((size_t)length + 1, 1);
   if (!bytes)
   {
      fclose(input);
      return 2;
   }
   size_t got = fread(bytes, 1, (size_t)length, input);
   fclose(input);
   aimee_request_t request = {0};
   cJSON *source = NULL, *before = NULL, *after = NULL;
   char *result = NULL;
   int rc = 2;
   if (got != (size_t)length)
      goto done;
   if (!strcmp(argv[1], "--offline-render-ir"))
   {
      source = aimee_ir_json_parse_frame(bytes, got);
      if (!source || aimee_ir_request_parse(source, &request))
         goto done;
      before = aimee_ir_request_json(&request);
      result = aimee_backend_render_wire(&request, wire);
      after = aimee_ir_request_json(&request);
      if (!result || !before || !after || !cJSON_Compare(before, after, 1))
         goto done;
   }
   else if (!strcmp(argv[1], "--offline-inspect-wire"))
   {
      char error[256];
      if (aimee_frontend_parse_wire(bytes, got, wire, &request, error, sizeof error))
         goto done;
      source = aimee_ir_request_json(&request);
      result = source ? cJSON_PrintUnformatted(source) : NULL;
      if (!result)
         goto done;
   }
   else
      goto done;
   int output = open(argv[4], O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
   if (output < 0)
      goto done;
   size_t size = strlen(result), written = 0;
   while (written < size)
   {
      ssize_t count = write(output, result + written, size - written);
      if (count <= 0)
         break;
      written += (size_t)count;
   }
   int closed = close(output);
   if (written != size || closed)
   {
      unlink(argv[4]);
      goto done;
   }
   printf("offline model codec: system=%d messages=%d tools=%d input_bytes=%ld output_bytes=%zu\n",
          request.n_system, request.n_messages, request.n_tools, length, size);
   printf("controls stream=%d max_present=%d temperature_present=%d top_p_present=%d "
          "top_k_present=%d tool_choice=%d cache_policy=%d metadata=%d provider_config=%d\n",
          request.stream, request.has_max_tokens, request.has_temperature, request.has_top_p,
          request.has_top_k, request.tool_choice, request.cache_policy, request.metadata != NULL,
          request.thinking != NULL);
   for (int i = 0; i < request.n_system; ++i)
      printf("system[%d] type=%d origin=%d authority=%d visible=%d cache=%d\n", i,
             request.system[i].type, request.system[i].context.origin,
             request.system[i].context.authority, aimee_ir_block_model_visible(&request.system[i]),
             request.system[i].cache_control != NULL);
   for (int i = 0; i < request.n_messages; ++i)
   {
      const aimee_message_t *m = &request.messages[i];
      int text = 0, thinking = 0, tools = 0, hidden = 0;
      for (int j = 0; j < m->n_blocks; ++j)
      {
         text += m->blocks[j].type == AIMEE_BLK_TEXT;
         thinking += m->blocks[j].type == AIMEE_BLK_THINKING;
         tools +=
             m->blocks[j].type == AIMEE_BLK_TOOL_USE || m->blocks[j].type == AIMEE_BLK_TOOL_RESULT;
         hidden += !aimee_ir_block_model_visible(&m->blocks[j]);
      }
      printf("message[%d] role=%s blocks=%d text=%d thinking=%d tool=%d hidden=%d compaction=%d\n",
             i, m->role, m->n_blocks, text, thinking, tools, hidden, m->compaction_boundary);
   }
   rc = 0;
done:
   if (rc)
      fputs("offline model codec refused invalid input/output; no transport attempted\n", stderr);
   free(result);
   free(bytes);
   cJSON_Delete(source);
   cJSON_Delete(before);
   cJSON_Delete(after);
   aimee_request_free(&request);
   return rc;
}
#endif
