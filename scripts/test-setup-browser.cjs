/* Built-browser release regressions. Run after npm --prefix frontend run build. */
const { chromium } = require('../frontend/node_modules/playwright');
const fs = require('fs');
(async()=>{
 const html=fs.readFileSync(process.argv[2] || require('path').join(__dirname,'../frontend/dist/index.html'),'utf8');
 const browser=await chromium.launch({headless:true,...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE ? {executablePath:process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE} : {}),args:['--no-sandbox']});
 try {
 for (const wizard of [false,true]) for (const lostResponse of [true,false]) {
  const context=await browser.newContext();
  await context.addInitScript(()=>localStorage.setItem('aimee_setup_dismissed','1'));
  const page=await context.newPage(); page.setDefaultTimeout(15000);
  const errors=[],batches=[];
  page.on('pageerror',e=>errors.push(e.message));
  const inventory=wizard?{projects:[],details:[]}:{projects:Array.from({length:73},(_,i)=>`existing/repo-${i}`),details:[]};
  const repos=['first','second','third'].map(name=>({name,clone_url:`https://github.com/clone-check/${name}.git`}));
  let recoveryObserved=false, lateRepo=null;
  const publish=repo=>{
   inventory.projects.push('clone-check/'+repo.name);
   inventory.details.push({ref:'clone-check/'+repo.name,org:'clone-check',name:repo.name,remote:repo.clone_url});
  };
  await page.route('http://gui-check.test/**',async route=>{
   const p=new URL(route.request().url()).pathname;
   let data={};
   if(!p.startsWith('/api/')) return route.fulfill({contentType:'text/html',body:html});
   if(p==='/api/auth/me') data={username:'admin'};
   if(p==='/api/chat/session') data={csrf:'test'};
   if(p==='/api/setup/account') data={complete:true};
   if(p==='/api/config') data={config:{provider:'test',embedder_model:'bekko-a25m',memory_backend_engine:'native'}};
   if(p==='/api/vault/credentials') data={credentials:[{agent:'git',cred:'author_name'},{agent:'git',cred:'author_email'}]};
   if(p==='/api/git/projects') {
    if(lateRepo && recoveryObserved) {publish(lateRepo);lateRepo=null;}
    data=inventory;
   }
   if(p==='/api/git/credentials') data={hosts:['github.com']};
   if(p==='/api/git/org-repos') data={provider:'github',repos};
   if(p==='/api/git/clone-org') {
    const body=route.request().postDataJSON(); batches.push(body.repos.map(r=>r.name));
    const repo=body.repos[0];
    if(repo.name==='second') {lateRepo=repo;return lostResponse ? route.abort('failed') : route.fulfill({status:503,contentType:'application/json',body:JSON.stringify({error:'aimee-server unavailable'})});}
    publish(repo);
    data={results:[{name:repo.name,ok:true,project:'clone-check/'+repo.name}]};
   }
   return route.fulfill({contentType:'application/json',body:JSON.stringify(data)});
  });
  await page.goto('http://gui-check.test/projects');
  await page.getByRole('button',{name:'Got it',exact:true}).click();
  if(wizard) {
   await page.evaluate(()=>window.dispatchEvent(new CustomEvent('aimee:open-setup-wizard')));
   await page.getByText('Workspaces & projects',{exact:true}).waitFor();
  }
  const scope=wizard?page.getByRole('dialog',{name:'Setup wizard'}):page;
  await scope.getByPlaceholder(wizard?/owner URL/:/repo URL/).fill('github.com/clone-check');
  await scope.getByRole('button',{name:'List repositories',exact:true}).click();
  await scope.getByRole('button',{name:'Clone selected (3)',exact:true}).click();
  await scope.getByRole('status').filter({hasText:'Checking completed clone second (2 of 3)'}).waitFor();
  // Inventory polling may overlap with reconciliation. Hold publication until
  // this assertion observes the checking state, independent of runner speed.
  recoveryObserved=true;
  await scope.getByRole('button',{name:'Clone selected (0)',exact:true}).waitFor();
  const cloned=await scope.getByText('cloned',{exact:true}).count();
  if(JSON.stringify(batches)!==JSON.stringify([['first'],['second'],['third']])||cloned!==3||errors.length) throw Error(JSON.stringify({wizard,batches,cloned,errors}));
  if(await scope.getByText(/unavailable|Could not confirm/).count()) throw Error('Spurious clone error remains');
  console.log(JSON.stringify({screen:wizard?'setup wizard':'Projects',failure:lostResponse?'lost response':'HTTP 503',batches,lateCloneRecovered:true,cloned,errors}));
  await context.close();
 }

 // The shipped wizard must present and persist every memory backend on first start.
 for (const engine of ['native', 'cognee', 'hillock']) {
  const context = await browser.newContext();
  await context.addInitScript(()=>localStorage.setItem('aimee_setup_dismissed','1'));
  const page = await context.newPage(); page.setDefaultTimeout(15000);
  const config = {provider:'test',embedder_model:'bekko-a25m'}, writes = [], errors = [];
  page.on('pageerror', e=>errors.push(e.message));
  await page.route('http://gui-check.test/**', async route=>{
   const path = new URL(route.request().url()).pathname;
   if (!path.startsWith('/api/')) return route.fulfill({contentType:'text/html',body:html});
   let data = {};
   if (path === '/api/auth/me') data={username:'admin'};
   if (path === '/api/chat/session') data={csrf:'test'};
   if (path === '/api/setup/account') data={complete:true};
   if (path === '/api/config') data={config};
   if (path === '/api/git/projects') data={projects:[],details:[]};
   if (path === '/api/git/credentials') data={hosts:['github.com']};
   if (path === '/api/vault/credentials') data={credentials:[{agent:'git',cred:'author_name'},{agent:'git',cred:'author_email'}]};
   if (path === '/api/config/set') {
    const body=route.request().postDataJSON(); writes.push(body);
    if (body.key === 'memory_backend_token') {config.memory_backend_token=true; data={value:true};}
    else if (body.key === 'memory.backend') {for(const [key,value] of Object.entries(body.value))config['memory_backend_'+key]=value;data={value:body.value,reload:'restart',applied_live:false};}
    else throw Error('Unexpected wizard write: '+body.key);
   }
   return route.fulfill({contentType:'application/json',body:JSON.stringify(data)});
  });
  await page.goto('http://gui-check.test/projects');
  await page.getByRole('button',{name:'Got it',exact:true}).click();
  await page.evaluate(()=>window.dispatchEvent(new CustomEvent('aimee:open-setup-wizard')));
  const dialog=page.getByRole('dialog',{name:'Setup wizard'});
  await dialog.getByLabel('Memory backend').selectOption(engine);
  if (engine !== 'native') {
   await dialog.getByLabel('Memory service origin').fill('http://'+engine+':8097');
   if(engine==='cognee')await dialog.getByLabel('Memory service authentication').selectOption('api-key');
   await dialog.getByLabel('Memory service credential').fill('fixture-browser-token');
  }
  await dialog.getByRole('button',{name:'Save memory backend',exact:true}).click();
  await dialog.getByText('Workspaces & projects',{exact:true}).waitFor();
  if(config.memory_backend_engine!==engine || writes.length!==(engine==='native'?1:2) || errors.length)throw Error(JSON.stringify({engine,writes,errors}));
  if(engine!=='native' && (writes[0].key!=='memory_backend_token' || writes[1].key!=='memory.backend'))throw Error('Credential was not sealed first');
  console.log(JSON.stringify({screen:'first-start memory',engine,credentialSealed:engine!=='native',atomicSelection:true,errors}));
  await context.close();
 }
 // A reinstall at the same origin must override a prior Finish flag.
 const context=await browser.newContext();
 await context.addInitScript(()=>localStorage.setItem('aimee_setup_dismissed','1'));
 const page=await context.newPage();page.setDefaultTimeout(15000);
 await page.route('http://gui-check.test/**',async route=>{
  const path=new URL(route.request().url()).pathname;
  if(!path.startsWith('/api/'))return route.fulfill({contentType:'text/html',body:html});
  const fixtures={
   '/api/auth/me':{username:'firstboot'},'/api/chat/session':{csrf:'test'},
   '/api/setup/account':{complete:false,username:'firstboot'},
   '/api/config':{config:{}},'/api/git/projects':{projects:[],details:[]},
   '/api/git/credentials':{hosts:[]},'/api/vault/credentials':{credentials:[]},
  };
  await route.fulfill({contentType:'application/json',body:JSON.stringify(fixtures[path]||{})});
 });
 await page.goto('http://gui-check.test/projects');
 await page.getByRole('dialog',{name:'Setup wizard'}).getByText('Secure your account',{exact:true}).waitFor();
 console.log(JSON.stringify({screen:'first boot',oldDismissalOverridden:true}));
 await context.close();
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exit(1)});
