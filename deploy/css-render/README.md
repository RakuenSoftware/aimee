# aimee css-render sidecar (#4-full render backend)

The render backend for the CSS migration assistant's **rendered computed-style
oracle** (#4-full). It turns `{html, css}` into a computed-style snapshot by
rendering in headless Chromium, so the oracle can diff the *computed* style of a
component before vs. after a conversion — the real correctness signal, stronger
than the static declaration-set oracle.

Because it renders **untrusted** application/exemplar markup in a browser engine
(a code-execution surface, proposal §8), it runs as an **isolated sidecar
container** that aimee-kb reaches only over HTTP. aimee never embeds a browser.

## How aimee talks to it

aimee's render adapter is the configured `css_render_command`: a shell command
that reads `{"html","css"}` on stdin and writes the snapshot JSON on stdout
(exactly like `embedding_command` drives the embedder). Point it at this sidecar
with `curl`:

```yaml
# aimee.yaml (aimee-kb)
css_style_graph_enabled: true
css_render_command: "curl -s --max-time 30 --data-binary @- http://aimee-css-render:8780/render"
```

With both set, aimee-kb registers the backend at startup and
`aimee css render-capture <project> <unit> <before|after> <html> <css>` renders +
stores a snapshot; `aimee css render-verify <project> <unit>` diffs before/after.

## Snapshot contract

Request: `POST /render  {"html":"...","css":"..."}`
Response:
```json
{"nodes":[{"ref":"<selector>","computed":{"<prop>":"<value>", ...}}, ...]}
```
Nodes captured = every element with a `data-ref` attribute (the migration
pipeline tags the elements it cares about). Properties = a fixed, deterministic
box/visual allowlist (see `render.js` `PROPS`) — kept small so diffs are stable
and snapshots bounded.

## Build & run

```sh
docker build -t aimee-css-render deploy/css-render
docker run --rm -p 8780:8780 --read-only --tmpfs /tmp aimee-css-render
```

Run it on an isolated network with no access to internal services (it executes
untrusted CSS/HTML). JavaScript is disabled in the render context; only `<style>`
+ markup are evaluated.
