/* Production-bundle webchat regressions. Run after npm --prefix frontend run build. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const { chromium } = require('../frontend/node_modules/playwright');

(async () => {
  const html = fs.readFileSync(path.join(__dirname, '../frontend/dist/index.html'), 'utf8');
  let sendResponse;
  let sent;
  const server = http.createServer((req, res) => {
    if (req.url !== '/api/chat/send') { res.writeHead(404).end(); return; }
    let body = '';
    req.on('data', chunk => { body += chunk; });
    req.on('end', () => {
      sent = JSON.parse(body);
      sendResponse = res;
      res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache' });
      res.write('event: turn_start\ndata: {}\n\n');
    });
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const origin = `http://127.0.0.1:${server.address().port}`;
  let browser;
  try {
    browser = await chromium.launch({ headless: true,
      ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : {}),
      args: ['--no-sandbox'],
    });
    const context = await browser.newContext();
    await context.addInitScript(() => {
      localStorage.setItem('aimee_setup_dismissed', '1');
      localStorage.setItem('aimee_tutorial_seen', JSON.stringify(['/chat', '/graph']));
    });
    const page = await context.newPage();
    page.setDefaultTimeout(10000);
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    let sessions = [
      { id: 'web-one', title: 'One', cwd: '/work/org/one', messages: [{ role: 'user', text: 'First conversation' }] },
      { id: 'web-two', title: 'Two', cwd: '/work/org/two', messages: [{ role: 'user', text: 'Second conversation' }] },
    ];
    let live = { changed: true, rev: 1, text: 'Reply in progress', status: 'active' };
    const bindings = [];
    await page.route(`${origin}/**`, async route => {
      const request = route.request();
      const url = new URL(request.url());
      const p = url.pathname;
      if (p === '/api/chat/send') return route.continue();
      if (!p.startsWith('/api/')) return route.fulfill({ contentType: 'text/html', body: html });
      if (p === '/api/chat/session-events') return route.fulfill({ contentType: 'text/event-stream', body: '' });
      if (p === '/api/chat/session' && request.method() === 'POST') {
        const change = request.postDataJSON();
        bindings.push(change);
        const existing = sessions.find(session => session.id === change.id);
        if (existing) { existing.title = change.title; existing.cwd = change.cwd; }
        else sessions.push({ id: change.id, title: change.title, cwd: change.cwd, messages: [] });
      }
      const fixtures = {
        '/api/auth/me': { username: 'alice' }, '/api/chat/session': { csrf: 'test' },
        '/api/chat/sessions': sessions, '/api/setup/account': { complete: true },
        '/api/config': { config: { provider: 'test', embedder_model: 'bekko-a25m' } },
        '/api/git/projects': { root: '/work', projects: ['org/one', 'org/two'], details: [] },
        '/api/chat/bootstrap-status': { has_rules: true }, '/api/chat/live': live,
        '/api/chat/attach': { attach_id: 'attachment' },
        '/api/git/credentials': { hosts: ['github.com'] },
        '/api/vault/credentials': { credentials: [{ agent: 'git', cred: 'author_name' }, { agent: 'git', cred: 'author_email' }] },
      };
      await route.fulfill({ status: p.startsWith('/api/sessions/workflows') ? 404 : 200,
        contentType: 'application/json', body: JSON.stringify(fixtures[p] || {}) });
    });
    await page.goto(`${origin}/chat`);
    await page.getByText('First conversation', { exact: true }).waitFor();
    assert.equal(await page.locator('nav').getByRole('link', { name: /Chat/ }).count(), 0);
    assert.equal(await page.getByRole('combobox').inputValue(), '/work/org/one');
    assert.equal(bindings.length, 0, 'Loading the project picker must not write a binding');

    await page.getByPlaceholder('Type a message… (Shift+Enter for newline)').fill('Please answer once');
    await page.getByPlaceholder('Type a message… (Shift+Enter for newline)').press('Enter');
    await page.getByText('Reply in progress', { exact: true }).waitFor();
    assert.equal(sent.cwd, '/work/org/one');
    assert.equal(sent.aimee_session_id, 'web-one');
    live = { changed: true, rev: 2, text: 'Reply from project one.', status: 'done' };
    sendResponse.end('event: turn_end\ndata: {}\n\n');
    await page.getByText('Reply from project one.', { exact: true }).waitFor();
    await page.getByRole('button', { name: 'Send', exact: true }).waitFor();
    assert.equal(await page.getByText('Reply from project one.', { exact: true }).count(), 1);
    assert.equal(await page.getByText('Reply in progress', { exact: true }).count(), 0);

    // A focus refresh returns metadata-only rows in a different order.
    sessions = [sessions[1], { ...sessions[0], messages: [] }];
    const refresh = page.waitForResponse(response => response.url().endsWith('/api/chat/sessions'));
    await page.evaluate(() => window.dispatchEvent(new Event('focus')));
    await refresh;
    await page.getByText('Reply from project one.', { exact: true }).waitFor();
    await page.locator('header').getByText('Two', { exact: false }).click();
    await page.getByText('Second conversation', { exact: true }).waitFor();
    assert.equal(await page.getByText('Reply from project one.', { exact: true }).count(), 0);
    await page.locator('header').getByText('One', { exact: false }).click();
    await page.getByText('Reply from project one.', { exact: true }).waitFor();

    await page.locator('nav').getByRole('link', { name: /Graph/ }).click();
    await page.getByRole('combobox').selectOption('/work/org/two');
    await page.locator('header').getByText('One', { exact: false }).click();
    await page.getByText('Reply from project one.', { exact: true }).waitFor();
    assert.equal(await page.getByRole('combobox').inputValue(), '/work/org/two');
    assert.equal(bindings.at(-1).cwd, '/work/org/two');
    await page.reload();
    await page.getByText('Reply from project one.', { exact: true }).waitFor();
    assert.equal(await page.getByRole('combobox').inputValue(), '/work/org/two');
    assert.deepEqual(errors, []);
    console.log('Webchat browser regressions passed: one final reply; history survives refresh, reordering, navigation, and reload; session project stays bound; top tabs open chat.');
  } finally {
    if (browser) await browser.close();
    server.closeAllConnections();
    await new Promise(resolve => server.close(resolve));
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
