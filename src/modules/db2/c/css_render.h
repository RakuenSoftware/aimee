/* db2/css_render.h: storage + evaluation for the #4-full rendered computed-style
 * oracle. The render adapter (a sandboxed headless browser, out-of-process)
 * produces a computed-style snapshot JSON per conversion unit per phase
 * (before/after); these are stored verbatim as the retained origin evidence and
 * diffed by css_render_oracle. The verdict is folded back into the existing
 * css_migration_units.oracle_equivalent column that already gates auto-accept.
 *
 * KB-side (DB2 owner). Gated by css_style_graph_enabled. */
#ifndef DEC_DB2_CSS_RENDER_H
#define DEC_DB2_CSS_RENDER_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      int available;     /* 0 -> verdict unknown (a before/after snapshot is missing) */
      int equivalent;    /* 1 iff available AND no computed-style diffs */
      int diff_count;    /* number of node/property diffs when available */
      char summary[256]; /* short human summary recorded on the unit note */
   } css_render_verdict_t;

   /* Store (upsert) a computed-style snapshot for (project, unit_path, phase).
    * phase must be "before" or "after". snapshot_json is retained verbatim (the
    * origin artifact). This is the seam where a sandboxed render backend's output
    * enters the system. Gated by css_style_graph_enabled: returns 0 (no-op) when
    * off, 1 when stored, -1 on error or invalid phase. */
   int db2_css_render_snapshot_store(const char *project, const char *unit_path, const char *phase,
                                     const char *snapshot_json, const char *now_iso);

   /* Fetch a stored snapshot JSON (caller frees *out). Returns 1 if found, 0 if
    * absent, -1 on error. */
   int db2_css_render_snapshot_get(const char *project, const char *unit_path, const char *phase,
                                   char **out);

   /* Load the before+after snapshots for a unit, run the rendered oracle, and
    * fold the verdict into css_migration_units.oracle_equivalent (+ a note). The
    * pipeline 'state' is NOT changed here. If a snapshot is missing the verdict is
    * available=0 and oracle_equivalent is set to -1 (unknown) — conservative.
    * Gated by css_style_graph_enabled. Returns 0 with *out filled on success,
    * -1 on error. */
   int db2_css_render_oracle_evaluate(const char *project, const char *unit_path,
                                      const char *now_iso, css_render_verdict_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_CSS_RENDER_H */
