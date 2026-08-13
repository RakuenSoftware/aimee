/* server_http_body.c: request-body buffer growth for the /v1 listener.
 *
 * Its own translation unit because server_http.c sits one line under the
 * line-check ceiling (2499 of 2500), so anything added there fails the build.
 * The concern is self-contained anyway, and it keeps company with the other
 * small HTTP helpers -- http_content_encoding.c, server_http_keepalive.c. */
#include "server_http_internal.h"
#include "server_conn_io.h"
#include "server.h"
#include <stdlib.h>
#include <string.h>

/* Grow *body to hold `need` bytes, doubling, never past `hard`. Returns 0 on
 * failure with *body still valid and owned by the caller.
 *
 * This exists because Content-Length is a CLAIM, not data. The body buffer used
 * to be malloc'd at the declared length before a single body byte was read, so a
 * request that merely SAID it was large reserved that much up front. Growing as
 * bytes actually arrive ties the allocation to real data; the declared length is
 * still the hard ceiling (checked against the route limit by the caller, and
 * passed in here as `hard`).
 *
 * WHAT THAT RESERVATION ACTUALLY COST, measured rather than assumed, because the
 * first version of this comment overstated it. It was ADDRESS SPACE, not
 * resident memory: a large malloc is served by mmap and untouched pages are
 * never faulted in, so a connection that declared 128MB and sent nothing cost
 * ~1.2MB RSS, not 128MB. Declaring 128MB and sending nothing:
 *
 *            resident peak    virtual peak
 *   before      1.20 MB         130.50 MB
 *   after       1.22 MB           2.50 MB
 *
 * So the win is the virtual reservation, which is what bites under strict
 * overcommit (vm.overcommit_memory=2) or an RLIMIT_AS, where a reservation that
 * is never touched still fails later allocations. On a default-overcommit box
 * the old code was untidy rather than dangerous -- worth fixing, but do not
 * repeat the claim that it pinned gigabytes of RAM.
 *
 * The cost side is negligible: a REAL 16MB body peaks at 17.36MB resident here
 * versus 17.26MB before (+0.6%), with identical virtual peak, because glibc
 * mremaps a large mmap'd chunk in place instead of copying it on each
 * doubling.
 *
 * The doubling guard is `next < hard - next` rather than `next * 2 < hard`, so
 * the size never overflows on the way up; when doubling cannot reach `need`, the
 * ceiling itself is used. */
static int http_body_reserve(char **body, size_t *cap, size_t need, size_t hard)
{
   if (need <= *cap)
      return 1;
   if (need > hard)
      return 0;
   size_t next = *cap ? *cap : (size_t)HTTP_BODY_INITIAL_ALLOC;
   while (next < need && next < hard - next)
      next *= 2;
   if (next < need)
      next = hard;
   if (next > hard)
      next = hard;
   char *grown = realloc(*body, next);
   if (!grown)
      return 0;
   *body = grown;
   *cap = next;
   return 1;
}

/* Read a `declared`-byte request body: the `prefix_len` bytes already sitting in
 * the header buffer, then whatever is still on the wire. Returns a
 * NUL-terminated buffer with the bytes ACTUALLY read in *out_len (which is short
 * of `declared` if the peer stopped early -- the caller decides whether that is
 * fatal), or NULL if the first slab could not be allocated.
 *
 * The caller has already rejected a `declared` over the route limit; this
 * function never allocates that much on the strength of the claim alone. */
char *http_read_body(int fd, const char *prefix, int prefix_len, int declared, int *out_len)
{
   size_t hard = (size_t)declared + 1;
   size_t cap = hard < HTTP_BODY_INITIAL_ALLOC ? hard : (size_t)HTTP_BODY_INITIAL_ALLOC;
   char *body = malloc(cap);
   if (!body)
      return NULL;

   int already = 0;
   if (prefix && prefix_len > 0)
   {
      if (!http_body_reserve(&body, &cap, (size_t)prefix_len + 1, hard))
      {
         free(body);
         return NULL;
      }
      memcpy(body, prefix, (size_t)prefix_len);
      already = prefix_len;
   }

   while (already < declared)
   {
      if (!http_body_reserve(&body, &cap, (size_t)already + 2, hard))
      {
         free(body);
         return NULL;
      }
      int want = (int)(cap - 1 - (size_t)already);
      if (want > declared - already)
         want = declared - already;
      if (want <= 0)
         break;
      int n = server_conn_io_read(fd, body + already, want);
      if (n <= 0)
         break;
      already += n;
   }

   body[already] = '\0';
   *out_len = already;
   return body;
}
