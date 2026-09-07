import json,pathlib
p=pathlib.Path('/opt/validation042')
paths=['evidence/'+x for x in ['T1/topology.json','T2/topology.json','T2/local-memory.json','T2/shared-memory.json','T2/identity.json','T3/topology.json','T3/local-memory.json','T3/semantic-memory.json','upgrade041/results.json','luks-T2/topology.json','luks-T2/local-memory.json','luks-T2/shared-memory.json','luks-T2/identity.json','luks-upgrade041/results.json','plain-storage.json','managed-storage.json','plain-host-observation.json','luks-host-setup.json','luks-fresh.json','luks-upgrade.json','model-lifecycle.json','model-probes.json','navigation.json','live-model-browser.json','exploratory.json','provider-recovery.json','container-recreation.json','final-runtime.json','fresh-environment.json','source-verification.json','core-images.json','browser-images.json','runtime-suite.json','browser-suite.json','recall-delay-published.json']]
paths+=['private/setup/result.json']+['private/providers/'+x+'.json' for x in ['exercise','after-restart','exploratory','module-down']]
def clean(v):
 if isinstance(v,dict):return {k:clean(vv) for k,vv in v.items() if k not in ('detail','response','stdout','stderr','observations','env','password','token','bootstrapPassword')}
 if isinstance(v,list):return [clean(x) for x in v]
 return v
print(json.dumps({x:clean(json.loads((p/x).read_text())) for x in paths if (p/x).exists()},indent=2,ensure_ascii=False))
