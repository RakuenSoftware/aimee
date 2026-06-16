/* oneshot.js: one-shot render CLI for aimee's #4-full computed-style oracle —
 * the ON-DEMAND (zero-idle) mode.
 *
 * Reads a {"html","css"} JSON object from STDIN, renders it in headless Chromium,
 * writes the computed-style snapshot JSON to STDOUT, and EXITS. So a container
 * exists only for the duration of one render — no idle Chromium. This is the
 * shape aimee's css_render_command expects directly (stdin -> stdout), so it can
 * be wired as a per-render ephemeral container, e.g.:
 *
 *   css_render_command: "docker run --rm -i aimee-css-render-oneshot"
 *
 * (see css-render-ctl.sh `render` and the README "On-demand" section). Renders
 * UNTRUSTED markup, so the container must stay isolated (proposal §8).
 *
 * Exit codes: 0 = snapshot on stdout; non-zero = error message on stderr (which
 * aimee surfaces as a render failure / UNKNOWN verdict — never a fake result).
 */
'use strict';

const { snapshot, closeBrowser } = require('./snapshot');

const MAX_INPUT = 8 * 1024 * 1024; // 8 MiB stdin cap

function readStdin() {
  return new Promise((resolve, reject) => {
    let data = '';
    let size = 0;
    process.stdin.setEncoding('utf8');
    process.stdin.on('data', (chunk) => {
      size += Buffer.byteLength(chunk);
      if (size > MAX_INPUT) { reject(new Error('stdin too large')); process.stdin.destroy(); return; }
      data += chunk;
    });
    process.stdin.on('end', () => resolve(data));
    process.stdin.on('error', reject);
  });
}

(async () => {
  try {
    const raw = await readStdin();
    const input = JSON.parse(raw || '{}');
    const snap = await snapshot(input.html || '', input.css || '');
    process.stdout.write(JSON.stringify(snap));
    await closeBrowser();
    process.exit(0);
  } catch (e) {
    process.stderr.write('css-render oneshot failed: ' + String(e && e.message || e) + '\n');
    try { await closeBrowser(); } catch (_) { /* ignore */ }
    process.exit(1);
  }
})();
