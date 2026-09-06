/* Run against an isolated real server/web deployment. See README.md. */
const { chromium } = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const base = process.env.PROVIDER_E2E_URL;
const fixture = process.env.PROVIDER_E2E_FIXTURE || 'http://127.0.0.1:18765';
const artifacts = process.env.PROVIDER_E2E_ARTIFACTS || '/tmp/aimee-provider-e2e-artifacts';
const prefix = process.env.PROVIDER_E2E_PREFIX || 'gui-e2e';
const phase = process.env.PROVIDER_E2E_PHASE || 'exercise';
const validationKind = process.env.PROVIDER_E2E_UI_SMOKE === '1' ? 'ui-smoke-with-test-responses' : 'real-stack';
const first = `${prefix}-a`, second = `${prefix}-b`;
if (!base) throw new Error('PROVIDER_E2E_URL must identify an isolated test deployment');

(async () => {
  await fs.mkdir(artifacts, { recursive: true });
  const browser = await chromium.launch({ headless: true, ...(process.env.CHROMIUM_PATH ? { executablePath: process.env.CHROMIUM_PATH } : {}) });
  const context = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: 1440, height: 1100 } });
  await context.addInitScript(() => {
    localStorage.setItem('aimee_setup_dismissed', '1');
    localStorage.setItem('aimee_tutorial_seen', JSON.stringify(['/providers', '/models', '/chat']));
  });
  const page = await context.newPage();
  page.setDefaultTimeout(15000);
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  const report = [];
  const pass = label => { report.push(label); console.log(`${validationKind}: PASS ${label}`); };
  async function api(url, body) {
    return page.evaluate(async ({ url, body }) => {
      const r = await fetch(url, { method: body === undefined ? 'GET' : 'POST', headers: {
        'Content-Type': 'application/json', 'X-CSRF-Token': window._csrf || '',
      }, ...(body === undefined ? {} : { body: JSON.stringify(body) }) });
      const data = await r.json();
      if (!r.ok || data.error || data.status === 'error') throw new Error(JSON.stringify(data));
      return data;
    }, { url, body });
  }
  async function providers() { return (await api('/api/providers')).providers; }
  async function modelRows() { return (await api('/api/models')).models; }
  async function ready(url) { await page.goto(`${base}${url}`); await page.getByRole('button', { name: 'Refresh', exact: true }).waitFor(); }
  async function providerAction(name, action) {
    // Select the smallest panel containing this provider's title and its actions.
    const title = page.getByText(name, { exact: true });
    const panel = title.locator('xpath=ancestor::*[.//button[normalize-space(.)="Edit provider"]][1]');
    await panel.getByRole('button', { name: action, exact: true }).click();
  }
  async function saveProvider() {
    const [response] = await Promise.all([page.waitForResponse(r => r.url().endsWith('/api/providers/save') && r.request().method() === 'POST'), page.getByRole('button', { name: 'Save provider', exact: true }).click()]);
    assert(response.ok(), await response.text());
    await page.getByLabel('Provider name', { exact: true }).waitFor({ state: 'detached' });
  }
  async function createProvider(name, key) {
    await page.getByRole('button', { name: '+ Add provider', exact: true }).click();
    await page.getByLabel('Provider name', { exact: true }).fill(name);
    await page.getByLabel('Endpoint', { exact: true }).fill(`${fixture}/shared/v1`);
    await page.getByLabel('API key', { exact: true }).fill(key);
    assert.equal(await page.getByLabel('Model ID', { exact: true }).count(), 0);
    await saveProvider();
    await page.getByText(name, { exact: true }).waitFor();
  }
  async function addModel(name) {
    await ready('/models');
    await page.getByRole('button', { name: '+ Add model', exact: true }).click();
    await page.getByRole('combobox', { name: 'Provider', exact: true }).selectOption(name);
    await page.getByRole('button', { name: 'Show models this provider offers', exact: true }).click();
    await page.getByRole('button', { name: 'fixture-model', exact: true }).click();
    await page.getByText('context not published', { exact: false }).waitFor();
    await page.getByRole('button', { name: 'Add model', exact: true }).click();
    await page.getByRole('button', { name: '+ Add model', exact: true }).waitFor();
  }
  async function removeProvider(name) {
    await providerAction(name, 'Delete provider');
    await Promise.all([page.waitForResponse(r => r.url().endsWith('/api/providers/remove') && r.request().method() === 'POST'), page.getByRole('button', { name: 'Confirm delete', exact: true }).click()]);
    await page.getByRole('button', { name: 'Confirm delete', exact: true }).waitFor({ state: 'detached' });
  }
  try {
    await page.goto(`${base}/providers`);
    if (page.url().includes('/login')) {
      if (!process.env.PROVIDER_E2E_USER || !process.env.PROVIDER_E2E_PASSWORD) throw new Error('Browser login credentials required');
      await page.locator('input[name=username]').fill(process.env.PROVIDER_E2E_USER);
      await page.locator('input[name=password]').fill(process.env.PROVIDER_E2E_PASSWORD);
      await page.getByRole('button', { name: 'Sign in' }).click();
      await page.waitForURL(url => !url.pathname.startsWith('/login'));
      pass('real browser login');
    }
    await ready('/providers');
    if (phase === 'exercise') {
      assert(!(await providers()).some(p => p.name === first || p.name === second), 'Use a fresh test prefix');
      await page.getByRole('button', { name: '+ Add provider', exact: true }).click();
      await page.getByLabel('Provider name', { exact: true }).fill('cancelled');
      await page.getByRole('button', { name: 'Cancel', exact: true }).click();
      assert(!(await providers()).some(p => p.name === 'cancelled'));
      await createProvider(first, 'fixture-key-a');
      await createProvider(second, 'fixture-key-b');
      assert.equal((await providers()).filter(p => [first, second].includes(p.name)).length, 2);
      assert.equal((await modelRows()).filter(m => [first, second].includes(m.registration)).length, 0);
      pass('two accounts at one endpoint, no models required; cancelled create persists nothing');

      await page.getByRole('button', { name: '+ Add provider', exact: true }).click();
      await page.getByLabel('Provider name', { exact: true }).fill(first);
      await page.getByLabel('Endpoint', { exact: true }).fill(`${fixture}/shared/v1`);
      await page.getByRole('button', { name: 'Save provider', exact: true }).click();
      await page.getByText('provider name already exists', { exact: false }).waitFor();
      await page.getByRole('button', { name: 'Cancel', exact: true }).click();
      assert.equal((await providers()).find(p => p.name === first).endpoint, `${fixture}/shared/v1`);
      pass('duplicate provider rejected without overwriting connection');
      await page.screenshot({ path: path.join(artifacts, 'providers-desktop.png'), fullPage: true });

      await addModel(first); await addModel(second);
      const models = (await modelRows()).filter(m => [first, second].includes(m.registration));
      assert.equal(models.length, 2);
      for (const row of models) {
        const result = await api('/api/models/probe', { args: [row.name] });
        assert.equal(result.execution_ok, true, JSON.stringify(result));
      }
      pass('model discovery and successful model-probe responses for both connections');
      const modelPanel = page.getByText(`${first}:fixture-model`, { exact: true })
        .locator('xpath=ancestor::*[.//button[normalize-space(.)="Edit"]][1]');
      await modelPanel.getByRole('button', { name: 'Edit', exact: true }).click();
      await page.getByLabel('context window (tok, 0 = auto)', { exact: true }).fill('40000');
      await page.getByLabel('max output (tokens, blank = auto)', { exact: true }).fill('4096');
      await page.getByLabel('input price ($/Mtok, blank = not stated)', { exact: true }).fill('0');
      await page.getByLabel('output price ($/Mtok, blank = not stated)', { exact: true }).fill('2.5');
      await Promise.all([page.waitForResponse(r => r.url().endsWith('/api/models/set') && r.request().method() === 'POST'), page.getByRole('button', { name: 'Save', exact: true }).click()]);
      await page.getByLabel('context window (tok, 0 = auto)', { exact: true }).waitFor({ state: 'detached' });
      let changedModel = (await modelRows()).find(m => m.registration === first);
      assert.equal(changedModel.context_window, 40000);
      assert.equal(changedModel.max_output, 4096);
      assert.equal(changedModel.price_in_per_mtok, 0);
      assert.equal(changedModel.price_in_declared, true);
      assert.equal(changedModel.price_out_per_mtok, 2.5);
      await page.reload();
      await modelPanel.getByRole('button', { name: 'Edit', exact: true }).click();
      await page.getByLabel('input price ($/Mtok, blank = not stated)', { exact: true }).fill('');
      await Promise.all([page.waitForResponse(r => r.url().endsWith('/api/models/set') && r.request().method() === 'POST'), page.getByRole('button', { name: 'Save', exact: true }).click()]);
      await page.getByLabel('context window (tok, 0 = auto)', { exact: true }).waitFor({ state: 'detached' });
      changedModel = (await modelRows()).find(m => m.registration === first);
      assert.equal(changedModel.price_in_declared, false);
      pass('model limits and prices editable on Models; declared free differs from blank');

      await page.screenshot({ path: path.join(artifacts, 'models-desktop.png'), fullPage: true });
      await ready('/providers');
      await providerAction(first, 'Edit provider');
      await page.getByLabel('Endpoint', { exact: true }).fill(`${fixture}/moved/v1`);
      await saveProvider();
      await api('/api/providers/models', { name: first });
      assert.equal((await modelRows()).find(m => m.registration === first).endpoint, `${fixture}/moved/v1`);
      assert.equal((await modelRows()).find(m => m.registration === second).endpoint, `${fixture}/shared/v1`);
      pass('endpoint edit applies to its model only; blank key preserved');
      await providerAction(first, 'Edit provider');
      await page.getByLabel('API key (leave blank to keep current)', { exact: true }).fill('fixture-key-rotated');
      await saveProvider();
      const probe = await api('/api/models/probe', { args: [`${first}:fixture-model`] });
      assert.equal(probe.execution_ok, true, JSON.stringify(probe));
      pass('rotated provider key used by existing model');
      await providerAction(second, 'Delete provider');
      await page.getByText(/1 attached model/).waitFor();
      await page.getByRole('button', { name: 'Cancel', exact: true }).click();
      assert((await providers()).some(p => p.name === second));
      pass('delete confirmation explains attached model and Cancel preserves it');
      await page.setViewportSize({ width: 390, height: 844 });
      await page.screenshot({ path: path.join(artifacts, 'providers-mobile.png'), fullPage: true });
      await page.setViewportSize({ width: 1440, height: 1100 });
      await page.reload();
      assert.equal((await providers()).filter(p => [first, second].includes(p.name)).length, 2);
      pass('hard refresh retains both providers');
      console.log('State retained for a real server/web restart. Run phase=after-restart next.');
    } else if (phase === 'after-restart') {
      assert.equal((await providers()).filter(p => [first, second].includes(p.name)).length, 2);
      assert.equal((await modelRows()).filter(m => [first, second].includes(m.registration)).length, 2);
      for (const name of [first, second]) {
        const probe = await api('/api/models/probe', { args: [`${name}:fixture-model`] });
        assert.equal(probe.execution_ok, true, JSON.stringify(probe));
      }
      pass('post-restart provider, model association, and credential checks');
      await removeProvider(first);
      assert(!(await modelRows()).some(m => m.registration === first));
      assert((await modelRows()).some(m => m.registration === second));
      await api('/api/models/remove', { args: [`${second}:fixture-model`] });
      await page.reload();
      assert((await providers()).some(p => p.name === second && p.model_count === 0));
      pass('provider deletion removes its models only; deleting last model retains provider');
      await removeProvider(second);
      await page.reload();
      assert(!(await providers()).some(p => [first, second].includes(p.name)));
      pass('empty provider can be deleted and stays deleted after refresh');
      await page.screenshot({ path: path.join(artifacts, 'providers-cleanup.png'), fullPage: true });
    } else throw new Error(`Unknown phase ${phase}`);
    assert.deepEqual(errors, [], 'Browser page errors');
    await fs.writeFile(path.join(artifacts, `${phase}.json`), JSON.stringify({ phase, validationKind, target: base, passed: report, pageErrors: errors }, null, 2));
  } catch (error) {
    await page.screenshot({ path: path.join(artifacts, `${phase}-failure.png`), fullPage: true });
    throw error;
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
