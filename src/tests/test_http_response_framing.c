/* Exercise the shipping buffered reader over real sockets, including binary
 * payloads that cannot be checked through the legacy NUL-terminated API. */
#include "../posix/agent_bridge.c"
#include <assert.h>

static void response(const char *name, const void *wire, size_t wire_length, int expected_status,
                     const void *expected_body, size_t expected_length, int keep_open)
{
   int pair[2];
   assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
   assert(fcntl(pair[0], F_SETFL, O_NONBLOCK) == 0);
   size_t sent = 0;
   while (sent < wire_length)
   {
      ssize_t n = write(pair[1], (const char *)wire + sent, wire_length - sent);
      assert(n > 0);
      sent += (size_t)n;
   }
   if (!keep_open)
      assert(!shutdown(pair[1], SHUT_WR));
   http_conn_t conn = {.fd = pair[0]};
   assert(aimee_core_control_init_timeout(&conn.control, keep_open ? 25 : 1000, 10, NULL, NULL) ==
          AIMEE_CORE_OK);
   char *body = NULL;
   size_t length = 0;
   char location[64] = "";
   int status =
       http_read_response_hdr(&conn, &body, &length, "Location", location, sizeof location);
   if (expected_status < 0)
   {
      if (status >= 0)
         fprintf(stderr, "%s: incomplete response returned HTTP %d (%zu body bytes)\n", name,
                 status, length);
      assert(status < 0);
      assert(!body && !length);
   }
   else
   {
      assert(status == expected_status && length == expected_length && body);
      assert(!length || (body && !memcmp(body, expected_body, length)));
      assert(!body || body[length] == '\0');
      if (!strcmp(name, "fixed binary"))
         assert(!strcmp(location, "/preserved-header"));
   }
   free(body);
   assert(!close(pair[0]));
   assert(!close(pair[1]));
   printf("  PASS: %s\n", name);
}

#define CASE(name, wire, status, expected, length, open)                                           \
   response(name, wire, sizeof(wire) - 1, status, expected, length, open)

int main(int argc, char **argv)
{
   if (argc == 2 && !strcmp(argv[1], "short-chunk-oob"))
   {
      CASE("short large chunk", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n100000\r\nx",
           -1, NULL, 0, 0);
      return 0;
   }
   CASE("fixed binary",
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nLocation: /preserved-header\r\n\r\na\0b\0c", 200,
        "a\0b\0c", 5, 0);
   CASE("fixed empty without EOF", "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 200, "", 0, 1);
   CASE("close-delimited binary", "HTTP/1.1 200 OK\r\n\r\na\0b", 200, "a\0b", 3, 0);
   CASE("binary chunks, extensions and trailers",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3 \t; part=one\r\na\0b\r\n"
        "2\r\n\0c\r\n0 \t; complete=yes\r\nX-Receipt: present\r\nAnother: value\r\n\r\n",
        200, "a\0b\0c", 5, 0);
   CASE("empty chunked response", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
        200, "", 0, 0);
   CASE("header values are not Transfer-Encoding fields",
        "HTTP/1.1 200 OK\r\nX-Debug: Transfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\nabc",
        200, "abc", 3, 0);
   CASE("chunked token uses complete field value",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunkedness\r\nContent-Length: 3\r\n\r\nabc", 200,
        "abc", 3, 0);
   CASE("chunked optional whitespace and case",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: \tChUnKeD \t\r\n\r\n3\r\nabc\r\n0\r\n\r\n", 200,
        "abc", 3, 0);
   CASE("header values are not Content-Length fields",
        "HTTP/1.1 200 OK\r\nX-Trace: Content-Length: 0\r\nContent-Length: 3\r\n\r\nabc", 200, "abc",
        3, 0);
   CASE("large header substring cannot select body length",
        "HTTP/1.1 200 OK\r\nX-Trace: Content-Length: 999\r\nContent-Length: 3\r\n\r\nabc", 200,
        "abc", 3, 0);
   CASE("case and optional whitespace", "HTTP/1.1 200 OK\r\ncOnTeNt-LeNgTh: \t003 \t\r\n\r\nabc",
        200, "abc", 3, 0);
   CASE("identical repeated Content-Length",
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 003\r\n\r\nabc", 200, "abc", 3, 0);
   CASE("identical Content-Length list", "HTTP/1.1 200 OK\r\nContent-Length: 3, 003\r\n\r\nabc",
        200, "abc", 3, 0);
   const char *bad_lengths[] = {
       "garbage", "", "   ", "-1", "+0", "3junk", "3.0", "18446744073709551616", "0,", "1,2"};
   for (size_t i = 0; i < sizeof bad_lengths / sizeof bad_lengths[0]; i++)
   {
      char wire[256];
      int n = snprintf(wire, sizeof wire, "HTTP/1.1 200 OK\r\nContent-Length: %s\r\n\r\nabc",
                       bad_lengths[i]);
      assert(n > 0 && (size_t)n < sizeof wire);
      response("malformed Content-Length", wire, (size_t)n, -1, NULL, 0, 0);
   }
   CASE("conflicting repeated Content-Length",
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab", -1, NULL, 0, 0);
   http_conn_t unused = {.fd = -1};
   parsed_url_t url = {0};
   snprintf(url.host, sizeof url.host, "fixture");
   snprintf(url.path, sizeof url.path, "/");
   volatile size_t impossible_length = SIZE_MAX;
   assert(send_request(&unused, "POST", &url, "application/octet-stream", NULL, NULL, "fixture",
                       "x", impossible_length) < 0);
   assert(send_request(&unused, "POST", &url, "application/octet-stream", NULL, NULL, "fixture",
                       "x", impossible_length - 4096) < 0);
   CASE("short declared body", "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nabc", -1, NULL, 0, 0);
   CASE("short chunk header", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3", -1, NULL, 0,
        0);
   CASE("short chunk body", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n8\r\nabc", -1,
        NULL, 0, 0);
   CASE("short large chunk", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n100000\r\nx", -1,
        NULL, 0, 0);
   CASE("short chunk CRLF", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r", -1,
        NULL, 0, 0);
   CASE("invalid chunk CRLF",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabcXX0\r\n\r\n", -1, NULL, 0, 0);
   CASE("missing final chunk", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n",
        -1, NULL, 0, 0);
   CASE("short zero chunk terminator", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n",
        -1, NULL, 0, 0);
   CASE("short trailer section",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nX-Receipt: present\r\n", -1,
        NULL, 0, 0);
   CASE("invalid chunk size", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nxyz\r\n\r\n",
        -1, NULL, 0, 0);
   CASE("positive sign is not a hex digit",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n+0\r\n\r\n", -1, NULL, 0, 0);
   CASE("negative zero is not a final chunk",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n-0\r\n\r\n", -1, NULL, 0, 0);
   CASE("hex prefix is not a chunk size",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0x0\r\n\r\n", -1, NULL, 0, 0);
   CASE("overflowing chunk size",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n10000000000000000\r\n\r\n", -1, NULL,
        0, 0);
   CASE("empty chunk size is not a final chunk",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n\r\n0\r\n\r\n", -1, NULL, 0, 0);
   CASE("short HTTP headers", "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n", -1, NULL, 0, 0);
   CASE("close-delimited timeout is not EOF", "HTTP/1.1 200 OK\r\n\r\npartial", -1, NULL, 0, 1);

   /* This crosses the reader's initial allocation and includes embedded NULs. */
   size_t size = 12000;
   char *wire = malloc(size + 128), *expected = malloc(size);
   assert(wire && expected);
   int header =
       snprintf(wire, 128, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n%zx\r\n", size);
   assert(header > 0 && header < 128);
   for (size_t i = 0; i < size; i++)
      expected[i] = (char)(i % 256);
   memcpy(wire + header, expected, size);
   static const char suffix[] = "\r\n0\r\n\r\n";
   memcpy(wire + header + size, suffix, sizeof suffix - 1);
   response("grown binary chunk", wire, (size_t)header + size + sizeof suffix - 1, 200, expected,
            size, 0);
   free(expected);
   free(wire);
   puts(
       "buffered HTTP framing: binary fidelity, complete boundaries and incomplete refusal passed");
   return 0;
}
