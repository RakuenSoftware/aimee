/* LIVE smoke test: real network, real DNS, real pages.
 *
 * Everything shipped so far (guarded egress, extraction, page cache, fusion)
 * has been verified against stubbed transports. This exercises the real thing
 * once, because a stubbed transport cannot tell you whether DNS resolution,
 * connection pinning, or SQLite persistence actually work in a deployment. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "db1_client/db1.h"
#include "web_egress.h"
#include "db1_client/web_page_cache.h"

void agent_http_init(void);
char *tool_web_read(const char *ref, const char *query, int span, const char *mode);
char *web_search_ex(const char *q, int n, int fetch_pages, const char *extract_query);

static int has(const char *h, const char *n){ return h && n && strstr(h,n)!=NULL; }

int main(void){
  int fail = 0;
  agent_http_init();   /* the server does this at startup; without it s_ssl_ctx
                        * is NULL and every HTTPS connection fails */

  /* 1. the deny-list, against REAL resolution */
  const char *err=NULL;
  char *loop = web_egress_fetch("http://127.0.0.1/", WEB_EGRESS_UNTRUSTED, NULL, 4000, 0, &err);
  printf("1. loopback refused          : %s (%s)\n", loop?"NO -- FAIL":"yes", err?err:"");
  if (loop) { fail=1; free(loop); }

  err=NULL;
  char *meta = web_egress_fetch("http://169.254.169.254/latest/meta-data/", WEB_EGRESS_UNTRUSTED, NULL, 4000, 0, &err);
  printf("2. metadata refused          : %s (%s)\n", meta?"NO -- FAIL":"yes", err?err:"");
  if (meta) { fail=1; free(meta); }

  /* 3. a real fetch + extraction, cold cache */
  const char *URL = "https://raw.githubusercontent.com/torvalds/linux/master/README";
  char *cold = tool_web_read(URL, "Linux kernel build", 0, NULL);
  int cold_ok = cold && has(cold, "untrusted retrieved content");
  printf("3. live fetch + extract      : %s (%zu bytes)\n", cold_ok?"yes":"NO -- FAIL", cold?strlen(cold):0);
  if (!cold_ok) { fail=1; if(cold) printf("   -> %.200s\n", cold); }

  /* 4. cache hit on the second read of the same URL */
  char *warm = tool_web_read(URL, "Linux kernel build", 0, NULL);
  int warm_ok = warm && has(warm, "served from cache");
  printf("4. second read served cached : %s\n", warm_ok?"yes":"NO -- FAIL");
  if (!warm_ok) { fail=1; if(warm) printf("   -> %.300s\n", warm); }

  /* 5. the cache is really in DB1, keyed by URL, and a DIFFERENT query hits it */
  long age=-1; char pin[64]="";
  char *stored = db1_web_page_get(URL, &age, pin, sizeof(pin));
  printf("5. stored in DB1             : %s (age=%lds pinned=%s)\n",
         stored?"yes":"NO -- FAIL", age, pin[0]?pin:"(none)");
  if (!stored) fail=1; else free(stored);

  char *other = tool_web_read(URL, "licence copyright", 0, NULL);
  int other_ok = other && has(other, "served from cache");
  printf("6. different query hits cache: %s\n", other_ok?"yes":"NO -- FAIL");
  if (!other_ok) fail=1;

  /* 7. redirects refused rather than followed */
  err=NULL;
  char *rd = web_egress_fetch("http://github.com/", WEB_EGRESS_UNTRUSTED, NULL, 6000, 0, &err);
  printf("7. redirect refused          : %s (%s)\n", rd?"followed -- CHECK":"yes", err?err:"");
  if (rd) free(rd);

  free(cold); free(warm); free(other);
  printf("\n%s\n", fail?"LIVE SMOKE: FAILURES ABOVE":"LIVE SMOKE: all core paths OK");
  return fail;
}
