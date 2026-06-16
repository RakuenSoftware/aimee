/* render.js: reference render backend for aimee's #4-full computed-style oracle.
 *
 * A headless-Chromium HTTP service. It renders UNTRUSTED app/exemplar markup, so
 * it is meant to run as an ISOLATED sidecar container (its own network/namespace)
 * that the aimee-kb reaches only over HTTP — never in a trusted aimee process
 * (proposal §8). aimee's `css_render_command` points at it, e.g.:
 *
 *   css_render_command: "curl -s --max-time 30 --data-binary @- http://aimee-css-render:8780/render"
 *
 * Protocol: POST /render  {"html": "...", "css": "..."}  ->  the snapshot shape
 * css_render_oracle expects:
 *   {"nodes":[{"ref":"<selector>","computed":{"<prop>":"<value>",...}}, ...]}
 *
 * Which nodes are captured: every element carrying a `data-ref` attribute (the
 * migration pipeline tags the elements it cares about). Which properties: a fixed
 * allowlist of box/visual properties (kept small + deterministic so the diff is
 * stable and the snapshot is bounded). Stdin/stdout is NOT used here; the curl
 * command bridges aimee's exec-pipe seam to this HTTP endpoint.
 */
'use strict';

const http = require('http');
const { chromium } = require('playwright');

const PORT = parseInt(process.env.CSS_RENDER_PORT || '8780', 10);
const MAX_BODY = 8 * 1024 * 1024; // 8 MiB request cap
const NAV_TIMEOUT_MS = parseInt(process.env.CSS_RENDER_TIMEOUT_MS || '15000', 10);

/* Deterministic, bounded property allowlist — the computed values that matter for
 * migration equivalence. Extend deliberately (every added property widens diffs). */
const PROPS = [
  'display', 'position', 'box-sizing',
  'width', 'height', 'margin-top', 'margin-right', 'margin-bottom', 'margin-left',
  'padding-top', 'padding-right', 'padding-bottom', 'padding-left',
  'border-top-width', 'border-right-width', 'border-bottom-width', 'border-left-width',
  'color', 'background-color', 'font-family', 'font-size', 'font-weight',
  'line-height', 'text-align', 'flex-direction', 'justify-content', 'align-items',
];

let browser = null;
async function getBrowser() {
  if (!browser) {
    browser = await chromium.launch({ args: ['--no-sandbox', '--disable-dev-shm-usage'] });
  }
  return browser;
}

async function snapshot(html, css) {
  const b = await getBrowser();
  const ctx = await b.newContext({ javaScriptEnabled: false });
  const page = await ctx.newPage();
  try {
    const doc = `<!doctype html><html><head><meta charset="utf-8"><style>${css || ''}</style></head><body>${html || ''}</body></html>`;
    await page.setContent(doc, { waitUntil: 'load', timeout: NAV_TIMEOUT_MS });
    const nodes = await page.evaluate((props) => {
      const out = [];
      for (const el of document.querySelectorAll('[data-ref]')) {
        const ref = el.getAttribute('data-ref');
        const cs = window.getComputedStyle(el);
        const computed = {};
        for (const p of props) computed[p] = cs.getPropertyValue(p);
        out.push({ ref, computed });
      }
      return out;
    }, PROPS);
    return { nodes };
  } finally {
    await ctx.close();
  }
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
      res.writeHead(200, { 'content-type': 'application/json' });
      res.end(JSON.stringify(snap));
    } catch (e) {
      res.writeHead(500, { 'content-type': 'application/json' });
      res.end(JSON.stringify({ error: String(e && e.message || e) }));
    }
  });
});

server.listen(PORT, () => { console.log(`aimee css-render listening on :${PORT}`); });
