/* Run against an isolated appliance with MEMORY_E2E_LOCAL_KEY and KB_KEY fixtures. */
const { chromium } = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
(async () => {
  const base = process.env.MEMORY_E2E_URL;
  const localKey = process.env.MEMORY_E2E_LOCAL_KEY;
  const kbKey = process.env.MEMORY_E2E_KB_KEY;
  assert(base && localKey && kbKey, 'URL and isolated memory fixture keys required');
  const output = process.env.MEMORY_E2E_OUTPUT || '/tmp/aimee-memory-browser';
  fs.mkdirSync(output, {recursive: true});
  const browser = await chromium.launch({headless: true});
  const page = await browser.newPage({ignoreHTTPSErrors: true, viewport: {width: 1280, height: 900}});
  const checks = [], errors = [];
  page.on('pageerror', e => errors.push(e.message));
  const pass = name => { checks.push({name, passed: true}); console.log('PASS '+name); };
  try {
    await page.goto(base+'/memory');
    if (page.url().includes('/login')) {
      await page.locator('input[name=username]').fill(process.env.MEMORY_E2E_USER);
      await page.locator('input[name=password]').fill(process.env.MEMORY_E2E_PASSWORD);
      await page.getByRole('button', {name:'Sign in', exact:true}).click();
      await page.waitForURL(url => !url.pathname.startsWith('/login'));
    }
    await page.goto(base+'/memory');
    await page.getByText(localKey, {exact:true}).waitFor();
    for (const name of ['Later', 'Got it']) {
      const button = page.getByRole('button', {name, exact:true});
      if (await button.isVisible()) await button.click();
    }
    assert.equal(await page.getByRole('combobox', {name:'Memory store'}).inputValue(), 'user');
    assert.equal(await page.getByText(kbKey, {exact:true}).count(), 0);
    pass('authenticated page defaults to local personal memories');
    await page.screenshot({path:output+'/personal.png', fullPage:true});
    await page.getByRole('combobox', {name:'Memory store'}).selectOption('kb');
    await page.getByText(kbKey, {exact:true}).waitFor();
    assert.equal(await page.getByText(localKey, {exact:true}).count(), 0);
    pass('KB selection replaces local results');
    await page.screenshot({path:output+'/kb.png', fullPage:true});
    await page.getByRole('combobox', {name:'Memory store'}).selectOption('user');
    const localRow = page.locator('article').filter({has:page.getByText(localKey, {exact:true})});
    await localRow.getByRole('button', {name:'Retire', exact:true}).click();
    await page.getByText(localKey, {exact:true}).waitFor({state:'detached'});
    await page.getByRole('combobox', {name:'Memory view'}).selectOption('retired');
    await page.getByText(localKey, {exact:true}).waitFor();
    pass('personal retirement removes active row and appears in retired view');
    await page.getByRole('combobox', {name:'Memory store'}).selectOption('kb');
    await page.getByText(kbKey, {exact:true}).waitFor();
    pass('personal retirement leaves colliding KB row active');
    await page.setViewportSize({width:390,height:844});
    await page.screenshot({path:output+'/kb-mobile.png', fullPage:true});
    assert.equal(errors.length,0,JSON.stringify(errors));
    pass('desktop and mobile render without JavaScript errors');
  } catch (e) {
    checks.push({name:e.message, passed:false});
    await page.screenshot({path:output+'/failure.png', fullPage:true});
    process.exitCode=1;
    console.error(e);
  } finally {
    fs.writeFileSync(output+'/result.json', JSON.stringify({checks,errors},null,2)+'\n');
    await browser.close();
  }
})();
