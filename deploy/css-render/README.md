# CSS render sidecar

This sidecar gives the CSS migration assistant a computed-style oracle. It renders untrusted HTML
and CSS in headless Chromium, then returns a bounded snapshot for before/after comparison. aimee-kb
never embeds a browser.

## Contract

`css_render_command` reads this on stdin:

```json
{"html":"...","css":"..."}
```

It writes:

```json
{"nodes":[{"ref":"header","computed":{"display":"flex"}}]}
```

Only elements with `data-ref` are captured. `snapshot.js` fixes the property allowlist so snapshots
stay small and stable.

Files:

- `snapshot.js`: render and capture;
- `render.js`: long-running `POST /render` server plus `GET /health`;
- `oneshot.js`: one render over stdin/stdout;
- `css-render-ctl.sh`: start, stop, inspect, reap, or run the container;
- `Dockerfile`: non-root Chromium image with page JavaScript disabled.

## Run it

Build once:

```bash
docker build -t aimee-css-render deploy/css-render
```

For a migration session, start the HTTP sidecar:

```bash
deploy/css-render/css-render-ctl.sh up
```

Point aimee-kb at it:

```yaml
css_style_graph_enabled: true
css_render_command: "curl -s --max-time 30 --data-binary @- http://host:8780/render"
```

Capture and compare:

```bash
aimee css render-capture <project> <unit> before before.html before.css
aimee css render-capture <project> <unit> after after.html after.css
aimee css render-verify <project> <unit>
```

Stop it when done:

```bash
deploy/css-render/css-render-ctl.sh down
```

For zero idle cost, run one container per render:

```yaml
css_render_command: "/path/to/css-render-ctl.sh render"
```

That path pays Chromium startup on every call and needs access to a container runtime. The HTTP
sidecar keeps Docker authority out of aimee-kb.

`css-render-ctl.sh` honors `DOCKER` and `DOCKER_HOST`. If the backend is down, the oracle reports
`UNAVAILABLE`; it does not invent a verdict.

Run the sidecar on an isolated network with no route to internal services. HTML and CSS are
untrusted input even with page JavaScript disabled.
