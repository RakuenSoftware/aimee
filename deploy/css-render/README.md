# aimee css-render sidecar (#4-full render backend)

The render backend for the CSS migration assistant's **rendered computed-style
oracle** (#4-full). It turns `{html, css}` into a computed-style snapshot by
rendering in headless Chromium, so the oracle can diff the *computed* style of a
component before vs. after a conversion — the real correctness signal, stronger
than the static declaration-set oracle.

Because it renders **untrusted** application/exemplar markup in a browser engine
(a code-execution surface, proposal §8), it runs as an **isolated container** that
aimee-kb reaches only over HTTP (or stdin/stdout). aimee never embeds a browser.

## How aimee talks to it

aimee's render adapter is the configured `css_render_command`: a shell command
that reads `{"html","css"}` on stdin and writes the snapshot JSON on stdout
(exactly like `embedding_command` drives the embedder). Two shapes:

- **HTTP sidecar** — a long-running container; the command `curl`s it.
- **One-shot** — an ephemeral container per render reading stdin → stdout → exit
  (`oneshot.js`); zero idle footprint.

With it configured, aimee-kb registers the backend at startup and
`aimee css render-capture <project> <unit> <before|after> <html> <css>` renders +
stores a snapshot; `aimee css render-verify <project> <unit>` diffs before/after.

## Files

- `snapshot.js` — shared headless render (the `[data-ref]` capture + property
  allowlist).
- `render.js` — long-running HTTP server (`POST /render`, `GET /health`).
- `oneshot.js` — stdin `{html,css}` → stdout snapshot → exit.
- `css-render-ctl.sh` — on-demand lifecycle (`up` / `down` / `status` / `reap` /
  `render`).
- `Dockerfile` — isolated, non-root, JS-disabled render context.

## Snapshot contract

Request: `{"html":"...","css":"..."}` → 
```json
{"nodes":[{"ref":"<selector>","computed":{"<prop>":"<value>", ...}}, ...]}
```
Nodes captured = every element with a `data-ref` attribute. Properties = a fixed
box/visual allowlist (`snapshot.js` `PROPS`) — small so diffs stay stable and
snapshots bounded.

## Build

```sh
docker build -t aimee-css-render deploy/css-render
```

## On-demand deployment (don't run Chromium 24/7)

Pick one of three patterns. All keep Chromium off except while migrating.

### 1. Session-scoped (recommended — no privilege change to aimee-kb)

Start the sidecar only around a migration session; aimee-kb just `curl`s it.

```sh
deploy/css-render/css-render-ctl.sh up            # before migrating
# aimee.yaml (aimee-kb):
#   css_style_graph_enabled: true
#   css_render_command: "curl -s --max-time 30 --data-binary @- http://<host>:8780/render"
# ... run `aimee css render-capture ...` / render-verify ...
deploy/css-render/css-render-ctl.sh down          # after
```

aimee-kb needs **no** container privileges — it only makes an HTTP call. The
oracle reports `UNAVAILABLE` (never a fake verdict) whenever the sidecar is down.

### 2. Lazy-start + idle-stop

As (1), but leave it warm during active work and let a cron reap it after idle
(render.js stamps a marker on every render):

```cron
*/5 * * * *  /path/css-render-ctl.sh reap 900     # stop after 15 min idle
```

### 3. Per-render ephemeral (zero idle)

A fresh container per render via `oneshot.js`. Point `css_render_command` straight
at it:

```yaml
css_render_command: "/path/css-render-ctl.sh render"   # == docker run --rm -i aimee-css-render node oneshot.js
```

This is the most "as-needed" (no idle container at all) but pays Chromium
cold-start (~1–3 s) per render, and **requires container-launch access wherever
`css_render_command` runs** (i.e. a Docker socket reachable from aimee-kb) — a
privilege surface to weigh vs. patterns 1–2.

### Non-default runtimes

`css-render-ctl.sh` honours `DOCKER` and `DOCKER_HOST`. For the smoothnas
LXC2Docker runtime:

```sh
DOCKER_HOST=unix:///run/smoothnas-runtime/docker.sock deploy/css-render/css-render-ctl.sh up
```

Run the sidecar on an isolated network with no access to internal services (it
executes untrusted CSS/HTML). JavaScript is disabled in the render context; only
`<style>` + markup are evaluated.
