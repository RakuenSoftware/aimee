/* db2/tool_registry.h: tool registry — DB2 subsystem.
 *
 * Stores per-tool metadata (input schema, side-effect class, enabled flag,
 * usage prompt). Read-mostly: DB2 schema bootstrap seeds known tools;
 * callers query via db2_tool_registry_lookup or iterate enabled
 * prompts via db2_tool_registry_iter_prompts. */
#ifndef DEC_DB2_TOOL_REGISTRY_H
#define DEC_DB2_TOOL_REGISTRY_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      char input_schema[8192];
      char side_effect[32];
      int enabled;
      int found;
   } tool_registry_entry_t;

   /* Look up a tool by name. Sets out->found = 1 if present.
    * Returns 0 on success, -1 on db error. */
   int db2_tool_registry_lookup(const char *name, tool_registry_entry_t *out);

   /* Get the side_effect classification for a tool. Returns "read" if
    * tool is missing or db is unavailable. The returned pointer references
    * a thread-local buffer; copy if you need to retain it across calls. */
   const char *db2_tool_registry_side_effect(const char *name);

   /* Visit each enabled (name, tool_prompt) pair.
    * cb receives raw tool_prompt from the table — empty when caller
    * should fall back to the embedded prompts table.
    * Iteration stops if cb returns non-zero; that value is returned. */
   typedef int (*tool_registry_prompt_cb)(const char *name, const char *prompt, void *user);
   int db2_tool_registry_iter_prompts(tool_registry_prompt_cb cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_TOOL_REGISTRY_H */
