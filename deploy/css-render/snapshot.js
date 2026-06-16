/* snapshot.js: the shared headless-Chromium render used by both the long-running
 * HTTP server (render.js) and the one-shot CLI (oneshot.js).
 *
 * Renders UNTRUSTED app/exemplar markup, so it must run inside an isolated
 * container (proposal §8). Produces the snapshot shape css_render_oracle expects:
 *   {"nodes":[{"ref":"<selector>","computed":{"<prop>":"<value>",...}}, ...]}
 * Nodes captured = every element carrying a `data-ref` attribute. Properties =
 * a fixed, deterministic box/visual allowlist (kept small so diffs stay stable
 * and snapshots bounded; extend deliberately).
 */
'use strict';

const { chromium } = require('playwright');

const NAV_TIMEOUT_MS = parseInt(process.env.CSS_RENDER_TIMEOUT_MS || '15000', 10);

const PROPS = [
  'display', 'position', 'box-sizing',
  'width', 'height', 'margin-top', 'margin-right', 'margin-bottom', 'margin-left',
  'padding-top', 'padding-right', 'padding-bottom', 'padding-left',
  'border-top-width', 'border-right-width', 'border-bottom-width', 'border-left-width',
  'color', 'background-color', 'font-family', 'font-size', 'font-weight',
  'line-height', 'text-align', 'flex-direction', 'justify-content', 'align-items',
];

let sharedBrowser = null;

/* Reuse one browser per process (the HTTP server renders many times); the
 * one-shot CLI launches, renders once, and closes it explicitly. */
async function getBrowser() {
  if (!sharedBrowser) {
    sharedBrowser = await chromium.launch({ args: ['--no-sandbox', '--disable-dev-shm-usage'] });
  }
  return sharedBrowser;
}

async function closeBrowser() {
  if (sharedBrowser) {
    const b = sharedBrowser;
    sharedBrowser = null;
    await b.close();
  }
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

module.exports = { snapshot, getBrowser, closeBrowser, PROPS };
