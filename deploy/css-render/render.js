/* render.js: long-running HTTP render backend for aimee's #4-full computed-style
 * oracle (the always-on / session-scoped sidecar mode).
 *
 * Headless-Chromium HTTP service rendering UNTRUSTED markup — run it as an
 * ISOLATED sidecar container the aimee-kb reaches only over HTTP (proposal §8).
 * aimee's `css_render_command` points at it, e.g.:
 *
 *   css_render_command: "curl -s --max-time 30 --data-binary @- http://aimee-css-render:8780/render"
 *
 * For a ZERO-IDLE, per-render container instead of a long-running one, see
 * oneshot.js + css-render-ctl.sh (on-demand mode). Both share snapshot.js.
 *
 * Protocol: POST /render {"html":"...","css":"..."} -> the snapshot shape
 * css_render_oracle expects. GET /health -> {"status":"ok"}.
 */
'use strict';

const http = require('http');
const fs = require('fs');
const { snapshot } = require('./snapshot');

const PORT = parseInt(process.env.CSS_RENDER_PORT || '8780', 10);
const MAX_BODY = 8 * 1024 * 1024; // 8 MiB request cap
// Touched on each render so an external reaper (css-render-ctl.sh reap) can stop
// an idle sidecar — the basis of the lazy-start / idle-stop on-demand pattern.
const LAST_RENDER_MARKER = process.env.CSS_RENDER_MARKER || '/tmp/.css-render-last';

function markRender() {
  try { fs.writeFileSync(LAST_RENDER_MARKER, String(Date.now())); } catch (_) { /* best effort */ }
}

const server = http.createServer((req, res) => {
  if (req.method === 'GET' && req.url === '/health') {
    res.writeHead(200, { 'content-type': 'application/json' });
    res.end('{"status":"ok"}');
    return;
  }
  if (req.method !== 'POST' || req.url !== '/render') {
    res.writeHead(404); res.end('not found');
    return;
  }
  let body = '';
  let aborted = false;
  req.on('data', (chunk) => {
    body += chunk;
    if (body.length > MAX_BODY) { aborted = true; res.writeHead(413); res.end('too large'); req.destroy(); }
  });
  req.on('end', async () => {
    if (aborted) return;
    let parsed;
    try { parsed = JSON.parse(body || '{}'); }
    catch (e) { res.writeHead(400, { 'content-type': 'application/json' }); res.end('{"error":"invalid JSON"}'); return; }
    try {
      const snap = await snapshot(parsed.html || '', parsed.css || '');
      markRender();
      res.writeHead(200, { 'content-type': 'application/json' });
      res.end(JSON.stringify(snap));
    } catch (e) {
      res.writeHead(500, { 'content-type': 'application/json' });
      res.end(JSON.stringify({ error: String(e && e.message || e) }));
    }
  });
});

server.listen(PORT, () => { console.log(`aimee css-render listening on :${PORT}`); });
