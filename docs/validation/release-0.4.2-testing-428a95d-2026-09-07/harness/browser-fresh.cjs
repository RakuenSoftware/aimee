/* Real isolated managed deployment; credentials JSON must be private to the tester.
 * Requires Playwright. Never prints login credentials, request bodies or KB tokens. */
const {chromium}=require('playwright');
const fs=require('node:fs');
const assert=require('node:assert/strict');
(async()=>{
 const credentials=JSON.parse(fs.readFileSync(process.env.SETUP_E2E_CREDENTIALS));
 const output=process.env.SETUP_E2E_OUTPUT || '/tmp/aimee-setup-browser';
 fs.mkdirSync(output,{recursive:true});
 const browser=await chromium.launch({headless:true,...(process.env.CHROMIUM_PATH?{executablePath:process.env.CHROMIUM_PATH}:{})});
 const page=await browser.newPage({ignoreHTTPSErrors:true,viewport:{width:1280,height:1000}});
 page.setDefaultTimeout(20000);
 const checks=[],errors=[];
 page.on('pageerror',e=>errors.push(e.message));
 const pass=name=>{checks.push({name,passed:true});console.log('PASS '+name);};
 try {
  await page.goto(credentials.url);
  await page.locator('input[name=username]').fill(credentials.user);
  await page.locator('input[name=password]').fill(credentials.password);
  await page.getByRole('button',{name:'Sign in',exact:true}).click();
  await page.waitForURL(u=>!u.pathname.startsWith('/login'));
  const wizard=page.getByRole('dialog',{name:'Setup wizard'});
  await wizard.waitFor();
  assert.equal(await page.getByText(/knowledge service is unreachable/).count(),0);
  pass('real login and healthy standalone browser without a KB warning');
  await wizard.getByRole('button',{name:'Create account & continue',exact:true}).waitFor();
  if(await wizard.getByRole('button',{name:'Create account & continue',exact:true}).count()) {
   const next={url:credentials.url,user:'release-validator',password:require('node:crypto').randomBytes(24).toString('hex')};
   fs.writeFileSync('/opt/validation042/private/replacement-account.json',JSON.stringify(next),{mode:0o600});
   await wizard.getByLabel('Username',{exact:true}).fill(next.user);
   await wizard.getByLabel('Password',{exact:true}).fill(next.password);
   await wizard.getByLabel('Confirm password',{exact:true}).fill(next.password);
   await wizard.getByRole('button',{name:'Create account & continue',exact:true}).click();
   await wizard.getByRole('button',{name:/OpenAI-compatible or local/}).waitFor();
   fs.writeFileSync(process.env.SETUP_E2E_CREDENTIALS,JSON.stringify(next),{mode:0o600});
   pass('fresh bootstrap account replaced through the real wizard');
  }
  await wizard.getByRole('button',{name:/OpenAI-compatible or local/}).click();
  await wizard.getByLabel('Endpoint',{exact:true}).fill('https://aimee-llm:8761');
  await wizard.getByLabel('Model',{exact:true}).fill('unsloth/gemma-4-E2B-it-qat-GGUF:qat-UD-Q4_K_XL');
  await wizard.getByRole('button',{name:'Save & set as primary',exact:true}).click();
  await wizard.getByText('Local memory models',{exact:true}).waitFor();
  pass('wizard accepts a local primary model without an API key');
  assert.equal(await wizard.getByText(/Deploy a local knowledge base|shared.store|PostgreSQL/).count(),0);
  await wizard.getByRole('combobox',{name:'synthesis',exact:true}).selectOption('gemma-4-E2B-it');
  await page.screenshot({path:output+'/local-models.png',fullPage:true,mask:[page.locator('pre')]});
  await wizard.getByRole('button',{name:'Save & continue',exact:true}).click();
  await wizard.getByRole('button',{name:/Skip for now/}).click();
  await wizard.getByRole('button',{name:'Continue without connecting',exact:true}).click();
  await wizard.getByRole('button',{name:'Continue',exact:true}).click();
  await wizard.getByRole('button',{name:/^(Re-deploy|Deploy)$/}).waitFor();
  assert.equal(await wizard.getByText(/Deploy the knowledge base/).count(),0);
  pass('wizard reaches local-model deployment with no KB installation or database step');
  await wizard.getByRole('button',{name:/^(Re-deploy|Deploy)$/}).click();
  await wizard.getByText(/Local model configuration applied/).waitFor({timeout:180000});
  pass('browser deploy starts the selected model services');
  await page.screenshot({path:output+'/deployed.png',fullPage:true,mask:[page.locator('pre')]});
  await wizard.getByRole('button',{name:'Finish',exact:true}).click();
  await page.goto(credentials.url+'/settings');
  await page.getByText(/Knowledge base/).first().waitFor();
  await page.setViewportSize({width:390,height:844});
  await page.screenshot({path:output+'/settings-mobile.png',fullPage:true,mask:[page.locator('pre')]});
  assert.equal(errors.length,0);
  pass('optional KB Settings remains reachable and mobile UI has no JavaScript errors');
 } catch(error) {
  checks.push({name:'browser setup completed',passed:false,error:error.message});
  console.error('FAIL browser setup; inspect private test artifacts');
  await page.screenshot({path:output+'/failure.png',fullPage:true,mask:[page.locator('pre')]}).catch(()=>{});
  fs.writeFileSync(output+'/failure-page.txt',await page.locator('body').innerText().catch(()=>''));
  process.exitCode=1;
 } finally {
  fs.writeFileSync(output+'/result.json',JSON.stringify({checks,errors},null,2)+'\n');
  await browser.close();
 }
})();
