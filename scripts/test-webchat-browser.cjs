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
  const histories = new Map(['web-one', 'web-two'].map((sid, index) => [sid, [{
    version: 1, session_id: sid, sequence: '1', event_id: `${sid}-1`, turn_id: 'initial', actor: 'alice',
    event: { kind: 'message', message_id: `${sid}-initial`, payload: { role: 'user', blocks: [{ type: 'text', text: index ? 'Second conversation' : 'First conversation' }] } },
  }]]));
  const subscribers = new Map();
  const connections = new Map();
  const encode = record => `id: ${record.sequence}\nevent: ir\ndata: ${JSON.stringify(record)}\n\n`;
  let currentTurn = 'browser-turn';
  function event(value) {
    const sid = sent.aimee_session_id;
    const history = histories.get(sid);
    const sequence = String(history.length + 1);
    const record = { version: 1, session_id: sid, sequence, event_id: `${sid}-${sequence}`, turn_id: currentTurn, actor: 'alice', event: value };
    history.push(record);
    for (const response of subscribers.get(sid) || []) response.write(encode(record));
    return encode(record);
  }
  const delta = (type, extra = {}) => ({ kind: 'delta', message_id: 'browser-answer', role: 'assistant', payload: { type, ...extra } });
  const server = http.createServer((req, res) => {
    const url = new URL(req.url, 'http://fixture');
    if (url.pathname === '/api/chat/session-events') {
      const sid = url.searchParams.get('sid');
      const history = histories.get(sid) || [];
      const cursor = BigInt(req.headers['last-event-id'] || url.searchParams.get('cursor') || '0');
      res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache' });
      res.flushHeaders();
      for (const record of history) if (BigInt(record.sequence) > cursor) res.write(encode(record));
      res.write(`event: replay_complete\ndata: ${JSON.stringify({session_id:sid,cursor:history.at(-1)?.sequence || '0'})}\n\n`);
      const streams = subscribers.get(sid) || new Set();
      streams.add(res); subscribers.set(sid, streams);
      connections.set(sid, (connections.get(sid) || 0) + 1);
      res.on('close', () => streams.delete(res));
      return;
    }
    if (req.url !== '/api/chat/send') { res.writeHead(404).end(); return; }
    let body = '';
    req.on('data', chunk => { body += chunk; });
    req.on('end', () => {
      sent = JSON.parse(body);
      sendResponse = res;
      res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache' });
      res.write(event({ kind: 'execution', payload: { status: 'running' } }));
      res.write(event({ kind: 'message', message_id: 'browser-user', payload: { role: 'user', blocks: [{ type: 'text', text: sent.message }] } }));
      res.write(event(delta('TURN_START')));
      res.write(event(delta('BLOCK_START', { block_id: 0, kind: 'text' })));
      res.write(event(delta('BLOCK_DELTA', { block_id: 0, kind: 'text', text_delta: 'Reply' })));
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
    const bindings = [];
    let controlReplies = 0;
    await page.route(`${origin}/**`, async route => {
      const request = route.request();
      const url = new URL(request.url());
      const p = url.pathname;
      if (p === '/api/chat/send' || p === '/api/chat/session-events') return route.continue();
      if (!p.startsWith('/api/')) return route.fulfill({ contentType: 'text/html', body: html });
      if (p === '/api/chat/control-reply') controlReplies++;
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
        '/api/chat/bootstrap-status': { has_rules: true },
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
    await page.getByText('Reply', { exact: true }).waitFor();
    assert.equal(sent.cwd, '/work/org/one');
    assert.equal(sent.aimee_session_id, 'web-one');
    sendResponse.write(event(delta('BLOCK_DELTA', { block_id: 0, kind: 'text', text_delta: ' from project one.' })));
    sendResponse.write(event(delta('BLOCK_STOP', { block_id: 0, kind: 'text' })));
    sendResponse.write(event(delta('TURN_STOP')));
    sendResponse.end(event({ kind: 'execution', payload: { status: 'completed' } }));
    await page.getByText('Reply from project one.', { exact: true }).waitFor();
    await page.getByRole('button', { name: 'Send', exact: true }).waitFor();
    assert.equal(await page.getByText('Reply from project one.', { exact: true }).count(), 1);
    assert.equal(await page.getByText('Reply', { exact: true }).count(), 0);
    const previousConnections = connections.get('web-one');
    const reconnect = page.waitForRequest(request => {
      const url = new URL(request.url());
      return url.pathname === '/api/chat/session-events' && url.searchParams.get('sid') === 'web-one';
    });
    for (const response of subscribers.get('web-one') || []) response.end();
    await reconnect;
    await page.getByText('Reply from project one.', { exact: true }).waitFor();
    assert.equal(await page.getByText('Reply from project one.', { exact: true }).count(), 1);
    assert.ok(connections.get('web-one') >= previousConnections);

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
    await page.setViewportSize({ width: 390, height: 844 });
    const conversation = await page.getByRole('log', { name: 'Conversation' }).boundingBox();
    const composer = await page.getByPlaceholder('Type a message… (Shift+Enter for newline)').boundingBox();
    assert.ok(conversation.width >= 350, `Mobile conversation was squeezed to ${conversation.width}px`);
    assert.ok(composer.width >= 250, `Mobile composer was squeezed to ${composer.width}px`);
    assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'Mobile page must not overflow horizontally');
    currentTurn = 'browser-question-turn';
    event({ kind: 'execution', payload: { status: 'running' } });
    const questionFrame = event({ kind: 'control_request', request_id: 'browser-question', control_kind: 'question',
      payload: { title: 'Choose the scope', questions: [{ id: 'scope', question: 'Which project should I inspect?', multiple: false,
        options: [{ label: 'Current project', description: 'Use the selected workspace.' }, { label: 'All projects' }] }] } });
    for (const response of subscribers.get('web-one') || []) response.write(questionFrame);
    const answer = page.getByRole('button', { name: 'Send answers', exact: true });
    await answer.waitFor();
    assert.equal(await answer.isDisabled(), true);
    await page.getByRole('radio', { name: /Current project/ }).check();
    assert.equal(await answer.isEnabled(), true);
    assert.equal(await page.getByText('Choose the scope', { exact: true }).count(), 1);
    assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'Question form must fit mobile viewport');
    await answer.click();
    await page.getByText('question: Reply sent', { exact: false }).waitFor();
    assert.equal(controlReplies, 1);
    event({ kind: 'control_response', request_id: 'browser-question', control_kind: 'question', payload: { decision: 'allow', answers: { scope: ['Current project'] } } });
    await page.getByText('Resolved requests (1)', { exact: true }).waitFor();
    assert.equal(await page.getByRole('button', { name: 'Send answers', exact: true }).count(), 0);
    assert.deepEqual(errors, []);
    console.log('Webchat browser regressions passed: canonical POST/SSE overlap and reconnect stay unique; history survives refresh, reordering, navigation and reload; projects stay bound; mobile conversation and composer remain usable.');
  } finally {
    if (browser) await browser.close();
    server.closeAllConnections();
    await new Promise(resolve => server.close(resolve));
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
