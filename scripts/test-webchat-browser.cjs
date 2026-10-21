/* Production-bundle webchat regressions. Run after npm --prefix frontend run build. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const crypto = require('node:crypto');
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
    let cancelMode = 'reject';
    const cancelRequests = [];
    let browserSendRequests = 0;
    page.on('request', request => { if (new URL(request.url()).pathname === '/api/chat/send' && request.method() === 'POST') browserSendRequests++; });
    let lastControlReply;
    let lookupAgent = '', lookupPersona = 'engineer';
    await page.route(`${origin}/**`, async route => {
      const request = route.request();
      const url = new URL(request.url());
      const p = url.pathname;
      if (p === '/api/chat/send' || p === '/api/chat/session-events') return route.continue();
      if (p === '/api/chat/cancel') {
        const body = request.postDataJSON();
        cancelRequests.push(body);
        return route.fulfill({ status: cancelMode === 'reject' ? 503 : 200, contentType: 'application/json',
          body: JSON.stringify(cancelMode === 'reject' ? { error: 'Unavailable' } :
            { status: 'ok', session_id: body.session_id, turn_id: body.turn_id, disposition: 'stopping' }) });
      }
      if (!p.startsWith('/api/')) return route.fulfill({ contentType: 'text/html', body: html });
      if (p === '/api/chat/control-reply') { controlReplies++; lastControlReply = request.postDataJSON(); }
      if ((p === '/api/chat/primary' || p === '/api/chat/persona') && request.method() === 'POST')
        return route.fulfill({contentType:'application/json',body:JSON.stringify(request.postDataJSON())});
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
        '/api/models': { default_agent: 'primary-model-with-a-long-name-for-responsive-layout', models: [{name:'primary-model-with-a-long-name-for-responsive-layout'}] },
        '/api/chat/personas': { personas: [{name:'engineer'},{name:'reviewer'}] },
        '/api/chat/attach': { session_id: p === '/api/chat/attach' ? request.postDataJSON().sid : '', attach_id: 'attachment' },
        '/api/chat/primary': { agent: lookupAgent }, '/api/chat/persona': { name: lookupPersona },
        '/api/git/credentials': { hosts: ['github.com'] },
        '/api/vault/credentials': { credentials: [{ agent: 'git', cred: 'author_name' }, { agent: 'git', cred: 'author_email' }] },
      };
      await route.fulfill({ status: p.startsWith('/api/sessions/workflows') ? 404 : 200,
        contentType: 'application/json', body: JSON.stringify(fixtures[p] || {}) });
    });
    async function assertPreferenceLayout(mobile) {
      const toolbar = page.locator('.chat-session-toolbar');
      const toolbarControls = [toolbar.locator('select'), toolbar.getByRole('button', {name:'Channels',exact:true}),
        toolbar.getByRole('button', {name:'Clear',exact:true}), toolbar.getByRole('button', {name:'+ Clone repo',exact:true})];
      const toolbarBoxes = await Promise.all(toolbarControls.map(control => control.boundingBox()));
      for (let i = 0; i < toolbarBoxes.length; i++) for (let j = i + 1; j < toolbarBoxes.length; j++) {
        const a = toolbarBoxes[i], b = toolbarBoxes[j];
        assert.ok(a.x + a.width <= b.x || b.x + b.width <= a.x || a.y + a.height <= b.y || b.y + b.height <= a.y,
          `Project and toolbar actions must not overlap at ${page.viewportSize().width}px: ${JSON.stringify({a,b})}`);
      }
      const footer = page.getByRole('group', {name:'Session preferences',exact:true});
      const viewport = page.viewportSize();
      const fields = [];
      for (const name of ['Agent','Persona']) {
        const field = footer.getByRole('combobox',{name,exact:true});
        await field.waitFor();
        const box = await field.boundingBox();
        const label = await footer.getByText(`${name}:`,{exact:true}).boundingBox();
        assert.ok(box.width >= 70 && box.x >= 0 && box.x + box.width <= viewport.width - 8,
          `${name} preference must remain visible inside the ${viewport.width}px viewport: ${JSON.stringify(box)}`);
        assert.ok(Math.abs(label.y + label.height / 2 - box.y - box.height / 2) <= 3,
          `${name} label must stay with its selector`);
        fields.push(box);
      }
      const tabs = await Promise.all(['Minimal','Standard','Extended'].map(name => footer.getByRole('tab',{name,exact:true}).boundingBox()));
      assert.ok(tabs.every(box => Math.abs(box.y - tabs[0].y) <= 1), 'Prompt tabs must stay horizontal');
      if (mobile) assert.ok(fields[1].y >= fields[0].y, 'Mobile preference groups must wrap in order');
      else assert.ok(fields.every(box => Math.abs(box.y - tabs[0].y) <= 3), 'Desktop preferences must fit one row');
    }
    await page.goto(`${origin}/chat`);
    await page.getByText('First conversation', { exact: true }).waitFor();
    assert.equal(await page.locator('nav').getByRole('link', { name: /Chat/ }).count(), 0);
    assert.equal(await page.locator('select:not([aria-label])').inputValue(), '/work/org/one');
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
    await page.locator('select:not([aria-label])').selectOption('/work/org/two');
    await page.locator('header').getByText('One', { exact: false }).click();
    await page.getByText('Reply from project one.', { exact: true }).waitFor();
    assert.equal(await page.locator('select:not([aria-label])').inputValue(), '/work/org/two');
    assert.equal(bindings.at(-1).cwd, '/work/org/two');
    await page.reload();
    await page.getByText('Reply from project one.', { exact: true }).waitFor();
    assert.equal(await page.locator('select:not([aria-label])').inputValue(), '/work/org/two');
    assert.deepEqual(errors, []);
    await page.setViewportSize({ width:1280,height:900 });
    await assertPreferenceLayout(false);
    await page.setViewportSize({width:320,height:844});
    await assertPreferenceLayout(true);
    assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), '320px preferences must not overflow horizontally');
    const personaWrite = page.waitForRequest(request => new URL(request.url()).pathname === '/api/chat/persona' && request.method() === 'POST');
    await page.getByRole('combobox',{name:'Persona',exact:true}).selectOption('reviewer');
    assert.equal((await personaWrite).postDataJSON().name,'reviewer');
    await page.waitForFunction(() => document.querySelector('select[aria-label="Persona"]')?.getAttribute('aria-busy') === 'false');

    await page.setViewportSize({ width: 390, height: 844 });
    await assertPreferenceLayout(true);
    const conversation = await page.getByRole('log', { name: 'Conversation' }).boundingBox();
    const composer = await page.getByPlaceholder('Type a message… (Shift+Enter for newline)').boundingBox();
    assert.ok(conversation.width >= 350, `Mobile conversation was squeezed to ${conversation.width}px`);
    assert.ok(composer.width >= 250, `Mobile composer was squeezed to ${composer.width}px`);
    assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'Mobile page must not overflow horizontally');
    currentTurn = 'browser-question-turn';
    event({ kind: 'execution', payload: { status: 'running' } });
    event({ kind: 'message', message_id: 'browser-tool-call', payload: { role: 'assistant', blocks: [
      { type: 'tool_use', tool_id: 'browser-inspect', name: 'inspect_workspace', input: { path: '/work/org/two' } },
    ] } });
    event({ kind: 'message', message_id: 'browser-tool-result', payload: { role: 'tool', blocks: [
      { type: 'tool_result', tool_id: 'browser-inspect', content: 'Read 12 project files. Canonical history is available for replay.' },
    ] } });
    await page.getByText('Tool: inspect_workspace', { exact: true }).click();
    await page.getByText('Output', { exact: true }).click();
    const output = page.getByText('Read 12 project files. Canonical history is available for replay.', { exact: true });
    await output.waitFor();
    const contrast = await output.evaluate(element => {
      const rgb = value => (value.match(/[\d.]+/g) || []).map(Number);
      const foreground = rgb(getComputedStyle(element).color);
      let parent = element;
      let background;
      while (parent) {
        const color = rgb(getComputedStyle(parent).backgroundColor);
        if (color.length === 3 || color[3] > 0) { background = color; break; }
        parent = parent.parentElement;
      }
      const luminance = color => color.slice(0, 3).map(channel => {
        const value = channel / 255;
        return value <= 0.04045 ? value / 12.92 : ((value + 0.055) / 1.055) ** 2.4;
      }).reduce((sum, value, index) => sum + value * [0.2126, 0.7152, 0.0722][index], 0);
      const light = luminance(background || [255, 255, 255]);
      const dark = luminance(foreground);
      return (Math.max(light, dark) + 0.05) / (Math.min(light, dark) + 0.05);
    });
    assert.ok(contrast >= 4.5, `Tool output contrast was only ${contrast.toFixed(2)}:1`);
    const questionFrame = event({ kind: 'control_request', request_id: 'browser-question', control_kind: 'question',
      payload: { title: 'Choose the scope', questions: [{ id: 'scope', question: 'Which project should I inspect?', multiple: false,
        options: [{ label: 'Current project', description: 'Use the selected workspace.' }, { label: 'All projects' }] }] } });
    for (const response of subscribers.get('web-one') || []) response.write(questionFrame);
    const answer = page.getByRole('button', { name: 'Send answers', exact: true });
    await answer.waitFor();
    const steeringButton = await page.getByRole('button', { name: 'Steer', exact: true }).boundingBox();
    const steeringComposer = await page.getByPlaceholder('Steer the running turn…').boundingBox();
    assert.ok(steeringButton.height <= 48, `Busy button grew to ${steeringButton.height}px`);
    assert.ok(steeringComposer.width >= 250, `Busy composer shrank to ${steeringComposer.width}px`);
    assert.equal(await answer.isDisabled(), true);
    await page.getByRole('radio', { name: /Current project/ }).check();
    assert.equal(await answer.isEnabled(), true);
    assert.equal(await page.getByText('Choose the scope', { exact: true }).count(), 1);
    assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'Question form must fit mobile viewport');
    await answer.click();
    await page.getByText('question: Reply sent', { exact: false }).waitFor();
    assert.equal(controlReplies, 1);
    const pendingReconnect = page.waitForRequest(request => new URL(request.url()).pathname === '/api/chat/session-events');
    for (const response of subscribers.get('web-one') || []) response.end();
    await pendingReconnect;
    await page.getByText('question: Reply sent', { exact: false }).waitFor();
    assert.equal(await answer.isDisabled(), true, 'Pending reply must stay disabled through reconnect');
    assert.equal(controlReplies, 1, 'Reconnect must not resubmit a question reply');
    event({ kind: 'control_response', request_id: 'browser-question', control_kind: 'question', payload: { decision: 'allow', answers: { scope: ['Current project'] } } });
    await page.getByText('Resolved requests (1)', { exact: true }).waitFor();
    assert.equal(await page.getByRole('button', { name: 'Send answers', exact: true }).count(), 0);
    lookupAgent = 'saved-agent-outside-the-current-catalog';
    lookupPersona = 'saved-persona-outside-the-current-catalog';
    await page.evaluate(() => window.dispatchEvent(new Event('focus')));
    await page.waitForFunction(() => document.querySelector('select[aria-label="Agent"]')?.value === 'saved-agent-outside-the-current-catalog' &&
      document.querySelector('select[aria-label="Persona"]')?.value === 'saved-persona-outside-the-current-catalog');
    for (const name of ['Agent', 'Persona']) assert.match(await page.getByRole('combobox', {name,exact:true}).locator('option:checked').textContent(), /not in current catalog/);
    await assertPreferenceLayout(true);
    assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'Saved out-of-catalog preferences must fit mobile viewport');
    lookupPersona = '';
    await page.evaluate(() => window.dispatchEvent(new Event('focus')));
    await page.waitForFunction(() => document.querySelector('select[aria-label="Persona"]')?.value === '');
    assert.equal(await page.getByRole('combobox', {name:'Persona',exact:true}).locator('option:checked').textContent(), 'Session default');
    event({ kind: 'message', message_id: 'interrupted-call', partial: true, native_owner: { version: 1, harness: 'codex', owner_id: 'a'.repeat(32), binding_sha256: 'b'.repeat(64), containment_sha256: 'c'.repeat(64), epoch: '0' }, payload: { role: 'assistant', blocks: [
      { type: 'tool_use', tool_id: 'lost-effect', name: 'write_file', input: { path: 'report.md' } },
    ] } });
    const interrupted = histories.get(sent.aimee_session_id).at(-1);
    await page.getByText('Running: write_file', { exact: true }).waitFor();
    event({ kind: 'execution', payload: { status: 'cancelled' } });
    const terminal = histories.get(sent.aimee_session_id).at(-1);
    const interruption = event({ kind: 'tool_interruption', payload: { version: 1,
      source: { session_id: interrupted.session_id, turn_id: interrupted.turn_id, sequence: interrupted.sequence,
        event_id: interrupted.event_id, actor: interrupted.actor, message_id: 'interrupted-call', block_index: 0,
        tool_id: 'lost-effect', name: 'write_file' },
      owner: { owner_id: 'a'.repeat(32), binding_sha256: 'b'.repeat(64), containment_sha256: 'c'.repeat(64), epoch: '0' },
      terminal_sequence: terminal.sequence, outcome: 'unknown', retry: 'explicit_permission_required' } });
    const unknown = page.getByText('Tool: write_file — outcome unknown', { exact: true });
    await unknown.waitFor();
    await unknown.click();
    await page.getByText("The host lost this tool's completion result.", { exact: false }).waitFor();
    const outcomeNote = page.getByRole('note');
    assert.equal(await outcomeNote.getAttribute('aria-live'), 'off', 'Historical unknown outcome is a passive note');
    const outcomeContrast = () => outcomeNote.evaluate(element => {
      const rgb = value => (value.match(/[\d.]+/g) || []).map(Number);
      const luminance = color => color.slice(0, 3).map(channel => {
        const value = channel / 255;
        return value <= 0.04045 ? value / 12.92 : ((value + 0.055) / 1.055) ** 2.4;
      }).reduce((sum, value, index) => sum + value * [0.2126, 0.7152, 0.0722][index], 0);
      const card = element.closest('details');
      const background = rgb(getComputedStyle(card).backgroundColor);
      const foreground = rgb(getComputedStyle(element).color);
      return (Math.max(luminance(background), luminance(foreground)) + 0.05) /
        (Math.min(luminance(background), luminance(foreground)) + 0.05);
    });
    const previousTheme = await page.evaluate(() => document.documentElement.getAttribute('data-theme'));
    for (const theme of ['light', 'dark']) {
      await page.evaluate(theme => { document.documentElement.dataset.theme = theme; }, theme);
      const warningContrast = await outcomeContrast();
      assert.ok(warningContrast >= 4.5, `${theme} unknown outcome note contrast was ${warningContrast.toFixed(2)}:1`);
    }
    await page.evaluate(theme => {
      if (theme === null) delete document.documentElement.dataset.theme;
      else document.documentElement.dataset.theme = theme;
    }, previousTheme);
    const cardColors = await page.evaluate(() => {
      const cards = [...document.querySelectorAll('.chat-messages details')];
      const unknown = cards.find(card => card.querySelector('p[role="note"]'));
      const ordinary = cards.find(card => card.querySelector('summary')?.textContent === 'Tool: inspect_workspace');
      return [getComputedStyle(unknown).backgroundColor, getComputedStyle(unknown).borderLeftColor,
        getComputedStyle(ordinary).backgroundColor, getComputedStyle(ordinary).borderLeftColor];
    });
    assert.notEqual(cardColors[0], cardColors[2], 'Unknown effect must have a distinct background');
    assert.notEqual(cardColors[1], cardColors[3], 'Unknown effect must have a distinct border');
    for (const response of subscribers.get(sent.aimee_session_id) || []) response.write(interruption);
    assert.equal(await unknown.count(), 1);
    assert.equal(controlReplies, 1);
    assert.equal(await page.getByRole('button', { name: 'Send answers', exact: true }).count(), 0);
    assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
    await page.getByText("The host lost this tool's completion result.", { exact: false }).scrollIntoViewIfNeeded();
    const metadataReconnect = page.waitForRequest(request => new URL(request.url()).pathname === '/api/chat/session-events');
    for (const response of subscribers.get(sent.aimee_session_id) || []) response.end();
    await metadataReconnect;
    await unknown.waitFor();
    assert.equal(await unknown.count(), 1);
    assert.equal(controlReplies, 1);
    await page.setViewportSize({ width: 1280, height: 900 });
    await page.getByText("The host lost this tool's completion result.", { exact: false }).scrollIntoViewIfNeeded();
    currentTurn = 'unknown-permission-turn';
    event({ kind: 'execution', payload: { status: 'running' } });
    const permissionDescription = "An earlier tool's effects are unknown. Allow this requested action once? This does not confirm or retry the earlier action automatically.";
    const permissionID = `${currentTurn}:unknown-effect:${'d'.repeat(64)}`;
    const permissionFrame = event({ kind: 'control_request', request_id: permissionID, control_kind: 'permission',
      payload: { tool: 'write_file', description: permissionDescription,
        input: { session_id: sent.aimee_session_id, turn_id: currentTurn, actor: 'private-actor', request_id: 'private-provider-request', attempt_id: 'private-host-attempt',
          tool: 'write_file', arguments: { path: 'new-report.md', content: 'A separately requested action' },
          unknown_receipts: { sha256: 'e'.repeat(64), count: 1, displayed: 1,
            sources: [{ event_id: 'private-receipt-event', session_id: sent.aimee_session_id, turn_id: 'private-old-turn', sequence: interrupted.sequence, tool_id: 'lost-effect', name: 'write_file' }] } } } });
    await page.getByText(permissionDescription, { exact: true }).waitFor();
    const permissionCard = page.locator('.ir-control').filter({ hasText: permissionDescription });
    assert.match(await permissionCard.textContent(), /1 earlier tool outcome remains unknown/);
    assert.match(await permissionCard.textContent(), /new-report\.md/);
    assert.match(await permissionCard.textContent(), /A separately requested action/);
    for (const hidden of [sent.aimee_session_id, 'private-actor', currentTurn, 'private-provider-request', 'private-host-attempt', 'private-receipt-event', 'e'.repeat(64), 'unknown_receipts', 'sha256'])
      assert.ok(!(await permissionCard.textContent()).includes(hidden), `Internal permission metadata shown: ${hidden}`);
    currentTurn = 'rejected-steer-turn';
    const steeringReason = 'Steering continuation was not dispatched; no model or tool executed. Send it again explicitly.';
    event({ kind: 'execution', payload: { status: 'running' } });
    event({ kind: 'execution', payload: { status: 'failed', error: JSON.stringify({ reason: steeringReason, dispatched: false,
      submitted_input_sha256: 'e'.repeat(64), submitted_input_bytes: 2000,
      submitted_input_preview: '<script>Do not execute this submitted text.</script>\nReview the changes first.', submitted_input_truncated: true }) } });
    await page.getByText(steeringReason, { exact: true }).waitFor();
    assert.equal(await page.getByText('Submitted text (shortened):', { exact: true }).count(), 1);
    assert.equal(await page.locator('blockquote').textContent(), '<script>Do not execute this submitted text.</script>\nReview the changes first.');
    assert.equal(await page.getByText('submitted_input_sha256', { exact: false }).count(), 0);
    for (const [name, width, theme] of [['desktop', 1280, 'light'], ['mobile', 390, 'light'], ['dark', 1280, 'dark']]) {
      await page.setViewportSize({ width, height: 1000 });
      await page.evaluate(theme => { document.documentElement.dataset.theme = theme; }, theme);
      await page.locator('.ir-control').filter({ hasText: permissionDescription }).scrollIntoViewIfNeeded();
      assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), `Permission and steering diagnostics overflow ${name}`);
      if (process.env.AIMEE_BROWSER_DIAGNOSTIC_SCREENSHOT_PREFIX)
        await page.screenshot({ path: `${process.env.AIMEE_BROWSER_DIAGNOSTIC_SCREENSHOT_PREFIX}-${name}.png` });
      await page.locator('blockquote').scrollIntoViewIfNeeded();
      if (process.env.AIMEE_BROWSER_DIAGNOSTIC_SCREENSHOT_PREFIX)
        await page.screenshot({ path: `${process.env.AIMEE_BROWSER_DIAGNOSTIC_SCREENSHOT_PREFIX}-steer-${name}.png` });
    }
    assert.equal(controlReplies, 1, 'Presenting diagnostics and a new permission must not send a reply');
    const decline = page.getByRole('button', { name: 'Decline', exact: true });
    await decline.click();
    await page.getByText('permission: Reply sent', { exact: false }).waitFor();
    assert.equal(controlReplies, 2);
    const permissionReconnect = page.waitForRequest(request => new URL(request.url()).pathname === '/api/chat/session-events');
    for (const response of subscribers.get(sent.aimee_session_id) || []) response.end();
    await permissionReconnect;
    await page.getByText('permission: Reply sent', { exact: false }).waitFor();
    assert.equal(await decline.isDisabled(), true);
    assert.equal(controlReplies, 2, 'Reconnect must not resend the declined permission');
    currentTurn = 'unknown-permission-turn';
    event({ kind: 'control_response', request_id: permissionID, control_kind: 'permission', payload: { decision: 'deny' } });
    event({ kind: 'execution', payload: { status: 'completed' } });
    for (const response of subscribers.get(sent.aimee_session_id) || []) response.write(permissionFrame);
    await page.getByText('Resolved requests (2)', { exact: true }).waitFor();
    assert.equal(await page.getByRole('button', { name: 'Allow once', exact: true }).count(), 0);
    assert.equal(controlReplies, 2, 'Duplicate historical permission cannot reopen or resubmit');
    const deniedReconnect = page.waitForRequest(request => new URL(request.url()).pathname === '/api/chat/session-events');
    for (const response of subscribers.get(sent.aimee_session_id) || []) response.end();
    await deniedReconnect;
    await page.getByText('Resolved requests (2)', { exact: true }).waitFor();
    assert.equal(await page.getByRole('button', { name: 'Allow once', exact: true }).count(), 0);
    assert.equal(controlReplies, 2);
    currentTurn = 'delegating-parent';
    event({ kind: 'execution', payload: { status: 'running' } });
    event({ kind: 'child_execution', payload: { session_id: 'private-browser-child', turn_id: 'child-turn', role: 'review', status: 'running' } });
    event({ kind: 'execution', payload: { status: 'completed' } });
    const childHistory = [];
    histories.set('private-browser-child', childHistory);
    const childEvent = value => {
      const record = { version: 1, session_id: 'private-browser-child', turn_id: 'child-turn', sequence: String(childHistory.length + 1),
        event_id: `child-${childHistory.length + 1}`, actor: 'alice', event: value };
      childHistory.push(record);
      for (const response of subscribers.get('private-browser-child') || []) response.write(encode(record));
    };
    childEvent({ kind: 'execution', payload: { status: 'running' } });
    childEvent({ kind: 'control_request', request_id: 'child-turn:question', control_kind: 'question', payload: { questions: [
      { id: 'answer', question: 'Child needs approval?', multiple: false, options: [] },
    ] } });
    const childDetails = page.getByText('Delegate: review — running', { exact: true });
    await childDetails.click();
    const childAnswer = page.getByRole('button', { name: 'Send answers', exact: true });
    await childAnswer.waitFor();
    await page.getByRole('textbox', { name: 'Your answer: Child needs approval?' }).fill('Approved child answer');
    await childAnswer.click();
    await page.getByText('question: Reply sent', { exact: false }).waitFor();
    assert.deepEqual(lastControlReply, { session_id: 'private-browser-child', request_id: 'child-turn:question',
      reply: { decision: 'allow', answers: { answer: ['Approved child answer'] } } });
    assert.equal(controlReplies, 3);
    await childDetails.click();
    await childDetails.click();
    await childAnswer.waitFor();
    assert.equal(await childAnswer.isDisabled(), true, 'Child collapse/reopen must retain accepted pending reply');
    assert.equal(controlReplies, 3);
    childEvent({ kind: 'control_response', request_id: 'child-turn:question', control_kind: 'question', payload: {
      decision: 'allow', answers: { answer: ['Approved child answer'] } } });
    childEvent({ kind: 'control_request', request_id: 'child-turn:late-permission', control_kind: 'permission', payload: { title: 'Child finishing request' } });
    await page.getByRole('button', { name: 'Allow once', exact: true }).waitFor();
    childEvent({ kind: 'execution', payload: { status: 'cancelled' } });
    await page.getByText('permission: Closed', { exact: false }).waitFor({ state: 'attached' });
    assert.equal(await page.getByRole('button', { name: 'Allow once', exact: true }).count(), 0);
    assert.equal(await page.getByRole('button', { name: 'Send answers', exact: true }).count(), 0);
    await childDetails.click();
    await childDetails.click();
    await page.getByText('permission: Closed', { exact: false }).waitFor({ state: 'attached' });
    assert.equal(await page.getByRole('button', { name: 'Allow once', exact: true }).count(), 0);
    assert.equal(controlReplies, 3, 'Child terminal replay must never reopen or resend controls');
    currentTurn = 'import-checkpoint';
    const archivedText = 'Original terminal text only: <script>never execute</script>\nTool-like text has no recoverable message role.';
    event({ kind: 'history', payload: { phase: 'archive', scope: 'legacy-archive', index: 0, archive_text: archivedText.slice(0, 30) } });
    event({ kind: 'history', payload: { phase: 'archive', scope: 'legacy-archive', index: 1, archive_text: archivedText.slice(30) } });
    assert.equal(await page.getByText('View archived terminal text', { exact: true }).count(), 0, 'Incomplete archive must remain hidden');
    const importReport = { version: 1, source_format: 'tmux_text', outcome: 'archived',
      source_sha256: crypto.createHash('sha256').update(archivedText).digest('hex'), source_session: 'private-legacy-source',
      source_agent: 'private-agent', source_provider: '', source_messages: 0, imported_messages: 0,
      removed_thinking_bindings: 0, tool_structure: 'unrecoverable', archive_chunks: 2 };
    const importFrame = event({ kind: 'history', payload: { phase: 'import_report', scope: 'legacy-archive', import_report: importReport } });
    await page.getByText('View archived terminal text', { exact: true }).waitFor();
    assert.equal(await page.getByText(archivedText, { exact: true }).count(), 0, 'Archive contents should mount only when requested');
    await page.getByText('View archived terminal text', { exact: true }).click();
    const archiveNote = page.getByRole('note').filter({ hasText: 'Legacy terminal text was archived outside model history.' });
    assert.equal(await archiveNote.locator('pre').textContent(), archivedText);
    assert.equal(await archiveNote.getAttribute('aria-live'), 'off');
    assert.equal(await archiveNote.locator('script').count(), 0);
    assert.ok(!(await archiveNote.textContent()).includes(importReport.source_sha256));
    assert.ok(!(await archiveNote.textContent()).includes(importReport.source_session));
    currentTurn = 'normalized-import-checkpoint';
    event({ kind: 'history', payload: { phase: 'import_report', scope: 'primary', import_report: { ...importReport,
      source_format: 'anthropic', outcome: 'normalized', source_messages: 2, imported_messages: 2,
      removed_thinking_bindings: 1, tool_structure: 'preserved', archive_chunks: 0 } } });
    await page.getByText('1 reasoning replay binding was removed.', { exact: true }).waitFor();
    const importedMessage = { role: 'user', blocks: [{ type: 'text', text: 'Original imported historical question' }] };
    event({ kind: 'history', payload: { phase: 'message', scope: 'primary', index: 0, message: importedMessage } });
    const largeImportedRaw = JSON.stringify({ role: 'assistant', blocks: [{ type: 'text', text: 'Verified fragmented historical answer 火' }] });
    const largeImportedHash = crypto.createHash('sha256').update(largeImportedRaw).digest('hex');
    for (const [index, text] of [largeImportedRaw.slice(0, 30), largeImportedRaw.slice(30)].entries())
      event({ kind: 'history', payload: { phase: 'message_fragment', scope: 'primary', fragment: {
        message_index: 1, part_index: index, part_count: 2, message_sha256: largeImportedHash, text } } });
    assert.equal(await page.getByText('Original imported historical question', { exact: true }).count(), 0, 'Import base must remain hidden until complete');
    event({ kind: 'history', payload: { phase: 'commit', scope: 'primary', count: 2 } });
    await page.getByText('Verified fragmented historical answer 火', { exact: true }).waitFor();
    assert.equal(await page.getByText('Original imported historical question', { exact: true }).count(), 1);

    assert.equal(controlReplies, 3);
    for (const response of subscribers.get(sent.aimee_session_id) || []) response.write(importFrame);
    const importReconnect = page.waitForRequest(request => new URL(request.url()).pathname === '/api/chat/session-events');
    for (const response of subscribers.get(sent.aimee_session_id) || []) response.end();
    await importReconnect;
    await page.getByText('1 reasoning replay binding was removed.', { exact: true }).waitFor();
    assert.equal(await page.getByText('View archived terminal text', { exact: true }).count(), 1);
    assert.equal(await page.getByText('Original imported historical question', { exact: true }).count(), 1);
    assert.equal(await page.getByText('Verified fragmented historical answer 火', { exact: true }).count(), 1);
    assert.equal(controlReplies, 3, 'Import/archive metadata replay cannot send control replies');
    assert.deepEqual(errors, []);
    currentTurn = 'explicit-stop-regression';
    event({ kind: 'execution', payload: { status: 'running' } });
    const stopDraft = page.getByPlaceholder('Steer the running turn…');
    await stopDraft.fill('Explicit next message after a confirmed stop');
    const beforeStopSends = browserSendRequests;
    await page.getByRole('button', { name: 'Steer', exact: true }).click();
    await page.getByText('Could not stop the selected turn. Your message is retained.', { exact: false }).waitFor();
    assert.equal(await stopDraft.inputValue(), 'Explicit next message after a confirmed stop');
    assert.equal(browserSendRequests, beforeStopSends, 'Rejected stop cannot send a continuation');
    cancelMode = 'stopping';
    await page.getByRole('button', { name: 'Steer', exact: true }).click();
    await page.waitForTimeout(150);
    assert.equal(cancelRequests.length, 2);
    assert.deepEqual(cancelRequests[1], { session_id: sent.aimee_session_id, turn_id: 'explicit-stop-regression' });
    assert.equal(browserSendRequests, beforeStopSends, 'Stop ACK must wait for canonical terminal');
    const nextSend = page.waitForRequest(request => new URL(request.url()).pathname === '/api/chat/send');
    event({ kind: 'execution', payload: { status: 'cancelled' } });
    await nextSend;
    assert.equal(browserSendRequests, beforeStopSends + 1);
    event({ kind: 'execution', payload: { status: 'cancelled' } });
    await page.waitForTimeout(150);
    assert.equal(browserSendRequests, beforeStopSends + 1, 'Terminal replay cannot repeat the continuation');
    assert.deepEqual(errors, []);
    console.log('Webchat browser regressions passed: canonical POST/SSE overlap and reconnect stay unique; history survives refresh, reordering, navigation and reload; projects stay bound; tool output remains readable; interrupted effects stay unknown through replay without control retries; mobile conversation/composer and wrapped session preferences remain usable.');
  } finally {
    if (browser) await browser.close();
    server.closeAllConnections();
    await new Promise(resolve => server.close(resolve));
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
