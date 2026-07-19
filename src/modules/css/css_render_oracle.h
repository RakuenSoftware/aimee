/* css_render_oracle.h: rendered computed-style equivalence oracle (#4-full core).
 *
 * The interim oracle (css_oracle.h) compares static declaration sets and is
 * STRICTLY WEAKER than the real correctness signal for a CSS migration, which is
 * the *computed style* of the rendered DOM before vs. after — cascade,
 * inheritance, and shorthand expansion already applied by a browser engine. This
 * module is the render-free CORE of that #4-full oracle: it operates over
 * computed-style SNAPSHOTS (per-node property->value maps produced by a real
 * render) and diffs them node-by-node, property-by-property.
 *
 * Producing the snapshots requires a headless browser running UNTRUSTED app/
 * exemplar code, which must run OUT-OF-PROCESS and SANDBOXED (proposal §8) — and
 * no such engine ships with aimee today. So the render itself is behind a
 * pluggable ADAPTER SEAM (css_render_oracle_set_adapter): a backend (a delegate-
 * driven renderer or a render sidecar) registers a function that turns html+css
 * into a snapshot JSON; with no adapter the oracle reports UNAVAILABLE rather
 * than guessing. The comparison logic here is a pure leaf (libc + cJSON) and is
 * fully testable without any browser.
 *
 * CONSERVATIVE, like the interim oracle: a missing/unparseable snapshot yields
 * available=0 (verdict unknown, never "equivalent"); unmatched nodes and any
 * property difference are reported as non-equivalence. Every result carries an
 * explicit limitation banner (no silent "equivalent", proposal §8). The snapshot
 * JSON (the captured computed styles) is the origin artifact and is retained by
 * the storage layer (see db2/css_render.*), never re-derived for diffing.
 */
#ifndef DEC_CSS_RENDER_ORACLE_H
#define DEC_CSS_RENDER_ORACLE_H 1

#include "css_analyze.h" /* CSS_PROPERTY_MAX, CSS_VALUE_MAX */

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define CSS_RENDER_REF_MAX 256 /* a node's stable ref (selector / DOM path) */

   /* One computed property of one rendered node. */
   typedef struct
   {
      char property[CSS_PROPERTY_MAX];
      char value[CSS_VALUE_MAX];
   } css_render_prop_t;

   /* One rendered node: a stable ref plus its computed-style map. */
   typedef struct
   {
      char ref[CSS_RENDER_REF_MAX];
      css_render_prop_t *props;
      int nprops;
   } css_render_node_t;

   /* A computed-style snapshot of one conversion unit (a set of nodes). Parsed
    * from the render adapter's JSON; the whole JSON is the retained origin. */
   typedef struct
   {
      css_render_node_t *nodes;
      int nnodes;
   } css_render_snapshot_t;

   typedef enum
   {
      CSS_RENDER_DIFF_CHANGED = 1,      /* node in both, computed value differs */
      CSS_RENDER_DIFF_REMOVED = 2,      /* property in BEFORE node, absent in AFTER node */
      CSS_RENDER_DIFF_ADDED = 3,        /* property in AFTER node, absent in BEFORE node */
      CSS_RENDER_DIFF_NODE_MISSING = 4, /* node ref in BEFORE, absent in AFTER (structural) */
      CSS_RENDER_DIFF_NODE_NEW = 5,     /* node ref in AFTER, absent in BEFORE (structural) */
   } css_render_diff_kind_t;

   typedef struct
   {
      char ref[CSS_RENDER_REF_MAX];
      char property[CSS_PROPERTY_MAX]; /* empty for NODE_MISSING / NODE_NEW */
      char before_value[CSS_VALUE_MAX];
      char after_value[CSS_VALUE_MAX];
      css_render_diff_kind_t kind;
   } css_render_diff_t;

   typedef struct
   {
      int available;  /* 0 iff a snapshot was missing/unparseable -> verdict unknown */
      int equivalent; /* 1 iff available AND every node's computed map is identical */
      css_render_diff_t *diffs;
      int diff_count;
      const char *limitation; /* static banner; not owned by the caller */
   } css_render_result_t;

   /* Parse a computed-style snapshot from the render adapter's JSON. Expected
    * shape: {"nodes":[{"ref":"<sel>","computed":{"<prop>":"<value>",...}},...]}.
    * Other top-level fields (viewport, states, ...) are ignored (forward-compat).
    * Returns NULL on allocation failure or malformed JSON. Free with
    * css_render_snapshot_free. */
   css_render_snapshot_t *css_render_snapshot_parse(const char *json);
   void css_render_snapshot_free(css_render_snapshot_t *s);

   /* Compare two computed-style snapshots node-by-node. If either is NULL the
    * result is available=0 / equivalent=0 (conservative: unknown != equivalent).
    * Returns NULL only on allocation failure. Free with css_render_result_free. */
   css_render_result_t *css_render_oracle_compare(const css_render_snapshot_t *before,
                                                  const css_render_snapshot_t *after);
   void css_render_result_free(css_render_result_t *r);

   /* The limitation banner text (also reachable via result->limitation). */
   const char *css_render_oracle_limitation_banner(void);

   /* ---- Render adapter seam -------------------------------------------------
    * A render backend implements this signature: given a unit's html + css,
    * produce a computed-style snapshot JSON (caller frees *out_json). Returns 0
    * on success; non-zero sets *err (caller frees) and leaves *out_json NULL.
    * The backend MUST run the browser engine out-of-process and sandboxed —
    * it renders untrusted code (proposal §8). */
   typedef int (*css_render_adapter_fn)(const char *html, const char *css, char **out_json,
                                        char **err);

   typedef enum
   {
      CSS_RENDER_OK = 0,
      CSS_RENDER_UNAVAILABLE = -1, /* no adapter registered — verdict stays unknown */
      CSS_RENDER_ERROR = -2,       /* adapter ran but failed (*err set) */
   } css_render_status_t;

   /* Register (or clear, with NULL) the process-wide render adapter. */
   void css_render_oracle_set_adapter(css_render_adapter_fn fn);
   int css_render_oracle_has_adapter(void);

   /* Render a unit via the registered adapter. With no adapter, returns
    * CSS_RENDER_UNAVAILABLE and leaves *out_json NULL — callers MUST treat that
    * as "unknown", never "equivalent". On CSS_RENDER_ERROR, *err is set. */
   css_render_status_t css_render_oracle_render(const char *html, const char *css, char **out_json,
                                                char **err);

#ifdef __cplusplus
}
#endif

#endif /* DEC_CSS_RENDER_ORACLE_H */
