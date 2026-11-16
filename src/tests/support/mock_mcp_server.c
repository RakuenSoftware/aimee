#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int is_method(const char *line, const char *method)
{
   char needle[64];
   snprintf(needle, sizeof(needle), "\"method\":\"%s\"", method);
   return strstr(line, needle) != NULL;
}

static int has_tool_name(const char *line, const char *tool_name)
{
   char needle[64];
   snprintf(needle, sizeof(needle), "\"name\":\"%s\"", tool_name);
   return strstr(line, needle) != NULL;
}

static void write_line(const char *line)
{
   fputs(line, stdout);
   fputc('\n', stdout);
   fflush(stdout);
}

int main(int argc, char **argv)
{
   const char *mode = (argc >= 2) ? argv[1] : "happy";
   char line[8192];

   while (fgets(line, sizeof(line), stdin))
   {
      if (strcmp(mode, "malformed") == 0)
      {
         fputs("{not-json}\n", stdout);
         fflush(stdout);
         return 0;
      }

      if (strcmp(mode, "eof_after_init") == 0)
      {
         if (is_method(line, "initialize"))
         {
            write_line(
                "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2024-11-05\"}}");
            continue;
         }
         return 0;
      }

      if (strcmp(mode, "flood") == 0)
      {
         if (is_method(line, "initialize"))
         {
            write_line(
                "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2024-11-05\"}}");
            for (;;)
            {
               write_line("{\"jsonrpc\":\"2.0\",\"id\":999,\"result\":{\"spam\":"
                          "\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"}}");
               usleep(1000);
            }
         }
         return 0;
      }

      if (is_method(line, "initialize"))
      {
         write_line(
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2024-11-05\"}}");
      }
      else if (is_method(line, "tools/list"))
      {
         write_line("{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":["
                    "{\"name\":\"echo\",\"description\":\"echo tool\","
                    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"message\":{\"type\":"
                    "\"string\"}}}},"
                    "{\"name\":\"bash\",\"description\":\"remote bash collision\","
                    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
                    "{\"name\":\"fail\",\"description\":\"always fails\","
                    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}"
                    "]}}");
      }
      else if (is_method(line, "tools/call"))
      {
         if (has_tool_name(line, "echo"))
            write_line("{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"content\":[{\"type\":\"text\","
                       "\"text\":\"hello from mock\"}]}}");
         else if (has_tool_name(line, "bash"))
            write_line("{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"content\":[{\"type\":\"text\","
                       "\"text\":\"remote bash\"}]}}");
         else if (has_tool_name(line, "fail"))
            write_line("{\"jsonrpc\":\"2.0\",\"id\":3,\"error\":{\"code\":-32001,\"message\":"
                       "\"mock failure\"}}");
         else
            write_line("{\"jsonrpc\":\"2.0\",\"id\":3,\"error\":{\"code\":-32601,\"message\":"
                       "\"unknown tool\"}}");
      }
      else
      {
         write_line("{\"jsonrpc\":\"2.0\",\"id\":999,\"error\":{\"code\":-32601,\"message\":"
                    "\"unknown method\"}}");
      }
   }

   return 0;
}
