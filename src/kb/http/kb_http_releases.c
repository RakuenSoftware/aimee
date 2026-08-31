#include "kb_http_releases.h"
#include "kb_http_ws.h"
#include "modules/db2/c/kb_docs.h"
#include "modules/db2/c/kb_releases.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int json_str_field(const char *json, const char *key, char *out, int cap)
{
   if (!json)
      return 0;
   char needle[256];
   snprintf(needle, sizeof(needle), "\"%s\":\"", key);
   const char *p = strstr(json, needle);
   if (!p)
      return 0;
   p += strlen(needle);
   int i = 0;
   while (*p && *p != '"' && i < cap - 1)
      out[i++] = *p++;
   out[i] = '\0';
   return 1;
}

static int64_t json_int64_field(const char *json, const char *key, int64_t def)
{
   if (!json)
      return def;
   char needle[256];
   snprintf(needle, sizeof(needle), "\"%s\":", key);
   const char *p = strstr(json, needle);
   if (!p)
      return def;
   p += strlen(needle);
   return (int64_t)atoll(p);
}

int handle_post_review_accept(const char *doc_id, const char *body, int body_len, char *out_buf,
                              int out_cap)
{
   (void)body_len;
   int64_t id = (int64_t)atoll(doc_id);
   if (id <= 0)
      return 400;
   if (db2_kb_doc_set_state(id, "accepted", 1, NULL) == -1)
      return 404;
   if (body)
   {
      int64_t rid = json_int64_field(body, "release_id", 0);
      if (rid > 0)
         db2_kb_release_add_doc(rid, id);
   }
   snprintf(out_buf, out_cap, "{\"doc_id\":%lld,\"state\":\"accepted\"}", (long long)id);
   return 200;
}

int handle_post_review_reject(const char *doc_id, const char *body, int body_len, char *out_buf,
                              int out_cap)
{
   (void)body_len;
   int64_t id = (int64_t)atoll(doc_id);
   if (id <= 0)
      return 400;
   char reason[256] = {0};
   json_str_field(body, "reason", reason, sizeof(reason));
   if (db2_kb_doc_set_state(id, "rejected", 0, reason[0] ? reason : NULL) == -1)
      return 404;
   snprintf(out_buf, out_cap, "{\"doc_id\":%lld,\"state\":\"rejected\"}", (long long)id);
   return 200;
}

int handle_post_releases(const char *body, int body_len, char *out_buf, int out_cap)
{
   (void)body_len;
   char name[128] = {0};
   json_str_field(body, "name", name, sizeof(name));
   if (!name[0])
      return 400;
   int64_t release_id = db2_kb_release_create(name);
   if (release_id == -1)
      return 409;
   snprintf(out_buf, out_cap, "{\"release_id\":%lld,\"state\":\"pending\"}", (long long)release_id);
   return 201;
}

int handle_post_promote(const char *release_id, char *out_buf, int out_cap)
{
   int64_t rid = (int64_t)atoll(release_id);
   if (rid <= 0)
      return 400;
   if (db2_kb_release_promote(rid) == -1)
   {
      snprintf(out_buf, out_cap, "{\"error\":\"promote failed\"}");
      return 409;
   }
   /* Active release changed → cached retrieval results are stale. */
   kb_ws_publish_invalidation("release", "global", NULL);
   snprintf(out_buf, out_cap, "{\"release_id\":%lld,\"state\":\"active\"}", (long long)rid);
   return 200;
}

int handle_post_rollback(const char *release_id, const char *body, int body_len, char *out_buf,
                         int out_cap)
{
   (void)body_len;
   int64_t rid = (int64_t)atoll(release_id);
   if (rid <= 0)
      return 400;
   int64_t target = json_int64_field(body, "target_release_id", 0);
   if (db2_kb_release_rollback(target) == -1)
   {
      snprintf(out_buf, out_cap, "{\"error\":\"no prior release to rollback to\"}");
      return 409;
   }
   kb_ws_publish_invalidation("release", "global", NULL);
   snprintf(out_buf, out_cap, "{\"state\":\"rolled_back\"}");
   return 200;
}

int handle_get_active_release(char *out_buf, int out_cap)
{
   int64_t id = db2_kb_release_get_active();
   if (id <= 0)
   {
      snprintf(out_buf, out_cap, "{\"active_release_id\":null}");
      return 200;
   }
   db2_kb_release_t rel;
   if (db2_kb_release_read(id, &rel) == -1)
   {
      snprintf(out_buf, out_cap, "{\"active_release_id\":null}");
      return 200;
   }
   snprintf(out_buf, out_cap,
            "{\"release_id\":%lld,\"name\":\"%s\",\"state\":\"active\",\"promoted_at\":\"%s\"}",
            (long long)rel.id, rel.name, rel.promoted_at);
   return 200;
}
