/* Negative-path checks against the disposable real stack used by browser.cjs. */
const { chromium } = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const base = process.env.PROVIDER_E2E_URL;
const name = `${process.env.PROVIDER_E2E_PREFIX || 'provider-exploratory'}-negative`;
const fixture = process.env.PROVIDER_E2E_FIXTURE || 'http://127.0.0.1:18765';
const dir = process.env.PROVIDER_E2E_ARTIFACTS || '/tmp/aimee-provider-e2e-artifacts';
(async () => {
 const browser = await chromium.launch({headless:true,...(process.env.CHROMIUM_PATH ? {executablePath:process.env.CHROMIUM_PATH}: {})});
 const context = await browser.newContext({ignoreHTTPSErrors:true,viewport:{width:1440,height:1000}});
 const page = await context.newPage(); page.setDefaultTimeout(45000);
 await context.addInitScript(() => {localStorage.setItem('aimee_setup_dismissed','1');localStorage.setItem('aimee_tutorial_seen',JSON.stringify(['/providers','/models','/chat']));});
 const results = [];
 const pass = label => {results.push(label);console.log(`PASS ${label}`);};
 async function request(url,body,csrf=true) {return page.evaluate(async ({url,body,csrf})=>{const r=await fetch(url,{method:body===undefined?'GET':'POST',headers:{'Content-Type':'application/json',...(csrf?{'X-CSRF-Token':window._csrf||''}:{})},...(body===undefined?{}:{body:JSON.stringify(body)})});const text=await r.text();let parsed;try{parsed=JSON.parse(text);}catch{parsed={error:text};}return {status:r.status,body:parsed};},{url,body,csrf});}
 try {
  await page.goto(`${base}/providers`);
  if(page.url().includes('/login')) {await page.locator('input[name=username]').fill(process.env.PROVIDER_E2E_USER);await page.locator('input[name=password]').fill(process.env.PROVIDER_E2E_PASSWORD);await page.getByRole('button',{name:'Sign in'}).click();await page.waitForURL(url=>!url.pathname.startsWith('/login'));}
  await page.goto(`${base}/providers`);
  if(process.env.PROVIDER_E2E_PHASE === 'module-down') {
   for(const url of ['/api/providers','/api/models']) {const r=await request(url);assert(r.status>=500,JSON.stringify(r));}
   await page.goto(`${base}/models`);await page.getByText(/model service unavailable|providers module unavailable/i).waitFor();
   pass('lost providers process returns errors for both pages, never an empty successful roster');
   await page.screenshot({path:`${dir}/models-module-down.png`,fullPage:true});
  } else {
   let r=await request('/api/providers');assert.equal(r.status,200);
   if(process.env.PROVIDER_E2E_RESET==='1' && r.body.providers.some(p=>p.name===name)) {await request('/api/providers/remove',{name,remove_models:true});r=await request('/api/providers');}
   assert(!r.body.providers.some(p=>p.name===name),'refusing to overwrite exploratory provider');
   const connection={name,provider:'openai',endpoint:`${fixture}/unavailable/v1`,auth_type:'bearer',api_key:'fixture-key-a',create:true};
   const cross=await context.request.post(`${base}/api/providers/save`,{headers:{Origin:'https://untrusted.example'},data:connection});assert.equal(cross.status(),403);pass('cross-origin request cannot create a provider');
   r=await request('/api/providers/save',connection);assert.equal(r.status,200,JSON.stringify(r));
   await page.goto(`${base}/models`);await page.getByRole('button',{name:'+ Add model',exact:true}).click();
   await page.getByRole('combobox',{name:'Provider',exact:true}).selectOption(name);
   await page.getByRole('button',{name:'Show models this provider offers',exact:true}).click();
   await page.getByText(/HTTP 503/).waitFor();
   await page.getByLabel('Model ID',{exact:true}).fill('manual-model');
   await page.getByRole('button',{name:'Add model',exact:true}).click();
   await page.getByRole('button',{name:'+ Add model',exact:true}).waitFor();
   r=await request('/api/models');assert(r.body.models.some(m=>m.name===`${name}:manual-model`));pass('unavailable discovery remains visible and manual model creation works');
   r=await request('/api/providers/save',{...connection,create:false,provider:'anthropic',api_key:'must-not-replace'});assert(r.status>=400);pass('attached model prevents an incompatible provider type change');
   r=await request('/api/providers/save',{...connection,create:false,endpoint:`${fixture}/shared/v1`,api_key:''});assert.equal(r.status,200,JSON.stringify(r));
   r=await request('/api/models/probe',{args:[`${name}:manual-model`]});assert.equal(r.body.execution_ok,true,JSON.stringify(r));pass('failed edit preserves credential and blank-key edit remains usable');
   r=await request('/api/providers/remove',{name,remove_models:true});assert.equal(r.status,200);
   r=await request('/api/providers/save',{...connection,endpoint:`${fixture}/shared/v1`,api_key:''});assert.equal(r.status,200);
   r=await request('/api/providers/models',{name});assert(r.status>=400,JSON.stringify(r));pass('delete and recreate without a key cannot reuse the deleted credential');
   r=await request('/api/providers/remove',{name});assert.equal(r.status,200);
   await page.goto(`${base}/providers`);await page.screenshot({path:`${dir}/providers-exploratory.png`,fullPage:true});
  }
  await fs.writeFile(`${dir}/${process.env.PROVIDER_E2E_PHASE==='module-down'?'module-down':'exploratory'}.json`,JSON.stringify({kind:'real-stack',results},null,2));
 } finally {await browser.close();}
})().catch(err=>{console.error(err);process.exitCode=1;});
