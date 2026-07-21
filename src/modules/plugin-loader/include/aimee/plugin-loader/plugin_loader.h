/* plugin_loader.h: four-source plugin discovery with last-writer-wins precedence.
 *
 * Part of the pluggable context engine surface described in
 * docs/proposals/pending/plugin-extension-surface-and-context-engine.md.
 *
 * Discovery order (lowest to highest priority):
 *   bundled  → <install_prefix>/plugins/
 *   user     → ~/.config/aimee/plugins/   (or $XDG_CONFIG_HOME/aimee/plugins/)
 *   project  → .aimee/plugins/            (gated: AIMEE_ENABLE_PROJECT_PLUGINS=1)
 *
 * A higher-priority source overrides a same-name plugin from a lower source.
 * Required-env checks are performed before loading; missing env skips the plugin.
 */
#ifndef DEC_PLUGIN_LOADER_H
#define DEC_PLUGIN_LOADER_H 1

#include "plugin.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* Set the install prefix used for bundled plugin discovery.
    * Call once at startup from main() before plugin_loader_discover_all().
    * If not called, falls back to $AIMEE_INSTALL_PREFIX, then ./plugins/ (cwd). */
   void plugin_loader_set_install_prefix(const char *prefix);

   /* Discover and register plugins from all sources (bundled, user, project).
    * Returns 0 on success.  Individual plugin failures are non-fatal; the
    * function continues and returns -1 only if a critical setup error occurs. */
   int plugin_loader_discover_all(char *err_buf, size_t err_len);

   /* Scan a single directory for plugin subdirectories.
    * Each subdir with a parseable manifest is added to out[].
    * Returns the number of plugins found (may be 0). */
   int plugin_loader_scan_dir(const char *dir, plugin_t *out, int max_out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_PLUGIN_LOADER_H */
