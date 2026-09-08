// Optional browser phase for client-pairing-e2e.py. Credentials arrive on stdin.
const { chromium } = require('playwright');
const assert = require('node:assert/strict');
let input = '';
process.stdin.on('data', chunk => input += chunk);
process.stdin.on('end', async () => {
  const { url, username, password } = JSON.parse(input);
  const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH || '/usr/bin/chromium', args: ['--no-sandbox'] });
  try {
    const context = await browser.newContext({ ignoreHTTPSErrors: true, permissions: ['clipboard-read', 'clipboard-write'] });
    const login = await context.request.post(url + '/api/auth/login', { data: { username, password } });
    assert.equal(login.status(), 200);
    const page = await context.newPage();
    await page.goto(url + '/settings');
    // The fixture completes its account step but leaves model setup optional.
    await page.getByRole('button', { name: 'Close setup', exact: true }).click();
    await page.getByLabel('Client name', { exact: true }).waitFor();
    await page.getByText('Workstation two', { exact: true }).waitFor();
    assert.equal(await page.getByText('Workstation two', { exact: true }).count(), 1);
    assert.equal(await page.getByText('Workstation one', { exact: true }).count(), 1);
    await page.getByLabel('Client name', { exact: true }).fill('Browser tablet');
    await page.getByRole('button', { name: 'Add client', exact: true }).click();
    await page.getByText('Browser tablet', { exact: true }).waitFor();
    await page.getByRole('button', { name: 'Copy pairing command', exact: true }).click();
    await page.getByRole('button', { name: 'Copied', exact: true }).waitFor();
    const command = await page.evaluate(() => navigator.clipboard.readText());
    assert.match(command, /^aimee remote set .* [0-9a-f]{64}$/);
    const row = page.locator('li').filter({ hasText: 'Browser tablet' });
    page.once('dialog', dialog => dialog.accept());
    await row.getByRole('button', { name: 'Revoke', exact: true }).click();
    await row.getByRole('button', { name: 'Revoke', exact: true }).waitFor({ state: 'detached' });
    assert.match(await row.innerText(), /revoked/);
    assert.equal(await page.getByRole('button', { name: 'Copy pairing command', exact: true }).count(), 0);
    console.log('PASS browser lists independent devices, creates and copies an invitation, and revokes only that invitation');
  } finally {
    await browser.close();
  }
});
