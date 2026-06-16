/* test_css_render_oracle.c: #4-full core — computed-style snapshot parsing,
 * node/property diffs, conservative unknown verdict, and the render adapter seam. */
#include "css_render_oracle.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const css_render_diff_t *find_diff(const css_render_result_t *r, const char *ref,
                                          const char *prop)
{
   for (int i = 0; i < r->diff_count; i++)
      if (strcmp(r->diffs[i].ref, ref) == 0 && strcmp(r->diffs[i].property, prop) == 0)
         return &r->diffs[i];
   return NULL;
}

static void test_identical_equivalent(void)
{
   const char *j = "{\"nodes\":[{\"ref\":\".btn\",\"computed\":"
                   "{\"color\":\"rgb(0, 0, 0)\",\"display\":\"inline-block\"}}]}";
   css_render_snapshot_t *a = css_render_snapshot_parse(j);
   css_render_snapshot_t *b = css_render_snapshot_parse(j);
   assert(a && b && a->nnodes == 1 && a->nodes[0].nprops == 2);
   css_render_result_t *r = css_render_oracle_compare(a, b);
   assert(r && r->available == 1 && r->equivalent == 1 && r->diff_count == 0);
   assert(r->limitation && strstr(r->limitation, "RENDERED"));
   css_render_result_free(r);
   css_render_snapshot_free(a);
   css_render_snapshot_free(b);
}

static void test_value_changed(void)
{
   css_render_snapshot_t *a = css_render_snapshot_parse(
       "{\"nodes\":[{\"ref\":\".btn\",\"computed\":{\"color\":\"rgb(0, 0, 0)\"}}]}");
   css_render_snapshot_t *b = css_render_snapshot_parse(
       "{\"nodes\":[{\"ref\":\".btn\",\"computed\":{\"color\":\"rgb(255, 0, 0)\"}}]}");
   css_render_result_t *r = css_render_oracle_compare(a, b);
   assert(r->available == 1 && r->equivalent == 0 && r->diff_count == 1);
   const css_render_diff_t *d = find_diff(r, ".btn", "color");
   assert(d && d->kind == CSS_RENDER_DIFF_CHANGED);
   assert(strcmp(d->before_value, "rgb(0, 0, 0)") == 0);
   assert(strcmp(d->after_value, "rgb(255, 0, 0)") == 0);
   css_render_result_free(r);
   css_render_snapshot_free(a);
   css_render_snapshot_free(b);
}

static void test_removed_and_added(void)
{
   css_render_snapshot_t *a = css_render_snapshot_parse(
       "{\"nodes\":[{\"ref\":\".x\",\"computed\":{\"color\":\"black\",\"margin\":\"0px\"}}]}");
   css_render_snapshot_t *b = css_render_snapshot_parse(
       "{\"nodes\":[{\"ref\":\".x\",\"computed\":{\"color\":\"black\",\"padding\":\"4px\"}}]}");
   css_render_result_t *r = css_render_oracle_compare(a, b);
   assert(r->equivalent == 0 && r->diff_count == 2);
   const css_render_diff_t *rem = find_diff(r, ".x", "margin");
   const css_render_diff_t *add = find_diff(r, ".x", "padding");
   assert(rem && rem->kind == CSS_RENDER_DIFF_REMOVED && strcmp(rem->before_value, "0px") == 0);
   assert(add && add->kind == CSS_RENDER_DIFF_ADDED && strcmp(add->after_value, "4px") == 0);
   css_render_result_free(r);
   css_render_snapshot_free(a);
   css_render_snapshot_free(b);
}

static void test_node_missing_and_new(void)
{
   css_render_snapshot_t *a = css_render_snapshot_parse(
       "{\"nodes\":[{\"ref\":\".a\",\"computed\":{\"color\":\"black\"}}]}");
   css_render_snapshot_t *b = css_render_snapshot_parse(
       "{\"nodes\":[{\"ref\":\".b\",\"computed\":{\"color\":\"black\"}}]}");
   css_render_result_t *r = css_render_oracle_compare(a, b);
   assert(r->equivalent == 0 && r->diff_count == 2);
   const css_render_diff_t *miss = find_diff(r, ".a", "");
   const css_render_diff_t *neu = find_diff(r, ".b", "");
   assert(miss && miss->kind == CSS_RENDER_DIFF_NODE_MISSING);
   assert(neu && neu->kind == CSS_RENDER_DIFF_NODE_NEW);
   css_render_result_free(r);
   css_render_snapshot_free(a);
   css_render_snapshot_free(b);
}

static void test_whitespace_normalized_equivalent(void)
{
   /* "rgb(0,  0,   0)" collapses to "rgb(0, 0, 0)" -> equivalent; property case
    * is normalized too (COLOR == color). */
   css_render_snapshot_t *a = css_render_snapshot_parse(
       "{\"nodes\":[{\"ref\":\".x\",\"computed\":{\"COLOR\":\"rgb(0,  0,   0)\"}}]}");
   css_render_snapshot_t *b = css_render_snapshot_parse(
       "{\"nodes\":[{\"ref\":\".x\",\"computed\":{\"color\":\"rgb(0, 0, 0)\"}}]}");
   css_render_result_t *r = css_render_oracle_compare(a, b);
   assert(r->available == 1 && r->equivalent == 1 && r->diff_count == 0);
   css_render_result_free(r);
   css_render_snapshot_free(a);
   css_render_snapshot_free(b);
}

static void test_missing_snapshot_is_unknown(void)
{
   css_render_snapshot_t *a = css_render_snapshot_parse(
       "{\"nodes\":[{\"ref\":\".x\",\"computed\":{\"color\":\"black\"}}]}");
   /* one side NULL (e.g. render unavailable) -> available=0, NOT equivalent */
   css_render_result_t *r = css_render_oracle_compare(a, NULL);
   assert(r->available == 0 && r->equivalent == 0);
   css_render_result_free(r);
   r = css_render_oracle_compare(NULL, NULL);
   assert(r->available == 0 && r->equivalent == 0);
   css_render_result_free(r);
   css_render_snapshot_free(a);
}

static void test_malformed_json_rejected(void)
{
   assert(css_render_snapshot_parse("not json") == NULL);
   assert(css_render_snapshot_parse("{\"nodes\":\"oops\"}") == NULL); /* nodes not array */
   assert(css_render_snapshot_parse("{}") == NULL);                   /* no nodes */
   assert(css_render_snapshot_parse(NULL) == NULL);
   /* a node missing ref/computed is skipped, not fatal */
   css_render_snapshot_t *s =
       css_render_snapshot_parse("{\"nodes\":[{\"ref\":\".ok\",\"computed\":{}},{\"bad\":1}]}");
   assert(s && s->nnodes == 1 && s->nodes[0].nprops == 0);
   css_render_snapshot_free(s);
}

/* ---- render adapter seam ------------------------------------------------- */

static int g_adapter_calls;

static int fake_adapter_ok(const char *html, const char *css, char **out_json, char **err)
{
   (void)html;
   (void)css;
   (void)err;
   g_adapter_calls++;
   const char *json = "{\"nodes\":[{\"ref\":\".x\",\"computed\":{\"color\":\"black\"}}]}";
   *out_json = strdup(json);
   return 0;
}

static int fake_adapter_fail(const char *html, const char *css, char **out_json, char **err)
{
   (void)html;
   (void)css;
   (void)out_json;
   *err = strdup("render boom");
   return 1;
}

static void test_adapter_seam(void)
{
   char *json = NULL, *err = NULL;
   /* No adapter registered by default -> UNAVAILABLE, never a fabricated result. */
   assert(!css_render_oracle_has_adapter());
   assert(css_render_oracle_render("<div/>", ".x{}", &json, &err) == CSS_RENDER_UNAVAILABLE);
   assert(json == NULL && err == NULL);

   /* Register a working backend -> OK + a parseable snapshot. */
   css_render_oracle_set_adapter(fake_adapter_ok);
   assert(css_render_oracle_has_adapter());
   g_adapter_calls = 0;
   assert(css_render_oracle_render("<div/>", ".x{}", &json, &err) == CSS_RENDER_OK);
   assert(g_adapter_calls == 1 && json != NULL && err == NULL);
   css_render_snapshot_t *s = css_render_snapshot_parse(json);
   assert(s && s->nnodes == 1);
   css_render_snapshot_free(s);
   free(json);
   json = NULL;

   /* A failing backend -> ERROR + err set, no snapshot. */
   css_render_oracle_set_adapter(fake_adapter_fail);
   assert(css_render_oracle_render("<div/>", ".x{}", &json, &err) == CSS_RENDER_ERROR);
   assert(json == NULL && err != NULL && strcmp(err, "render boom") == 0);
   free(err);
   err = NULL;

   /* Clearing returns to UNAVAILABLE. */
   css_render_oracle_set_adapter(NULL);
   assert(!css_render_oracle_has_adapter());
   assert(css_render_oracle_render("<div/>", ".x{}", &json, &err) == CSS_RENDER_UNAVAILABLE);
}

int main(void)
{
   test_identical_equivalent();
   test_value_changed();
   test_removed_and_added();
   test_node_missing_and_new();
   test_whitespace_normalized_equivalent();
   test_missing_snapshot_is_unknown();
   test_malformed_json_rejected();
   test_adapter_seam();
   printf("css_render_oracle: all tests passed\n");
   return 0;
}
