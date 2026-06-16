/* css_render_cmd.h: command-driven render backend for the #4-full rendered
 * computed-style oracle (slice 3).
 *
 * Implements the css_render_oracle adapter seam by shelling out to the configured
 * `css_render_command` (mirrors how embedding_command drives the embedder): the
 * command reads a {"html","css"} JSON object on stdin and writes a computed-style
 * snapshot JSON on stdout. Because it renders UNTRUSTED markup in a browser
 * engine, the command MUST be an isolated, out-of-process backend — typically a
 * `curl` to a sandboxed render sidecar container (proposal §8). This subsumes
 * both the "delegate/local script" and "render sidecar" options behind one
 * config string; aimee never embeds a browser.
 */
#ifndef DEC_CSS_RENDER_CMD_H
#define DEC_CSS_RENDER_CMD_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Register the command-driven adapter as the process render backend, IFF both
    * css_style_graph_enabled is set AND css_render_command is non-empty. Called
    * once at aimee-kb startup. No-op (leaving the oracle UNAVAILABLE) otherwise.
    * Returns 1 if an adapter was registered, 0 if not. */
   int css_render_cmd_register(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_CSS_RENDER_CMD_H */
