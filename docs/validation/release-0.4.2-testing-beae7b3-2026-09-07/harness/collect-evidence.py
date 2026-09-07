import json,pathlib
p=pathlib.Path('/opt/validation042')
paths=['evidence/'+x for x in ['T1/topology.json','T2/topology.json','T2/local-memory.json','T2/shared-memory.json','T2/identity.json','T3/topology.json','T3/local-memory.json','T3/semantic-memory.json','upgrade041/results.json','luks-fresh.json','luks-upgrade.json','model-lifecycle.json','model-isolated-1.json','model-isolated-2.json','model-isolated-3.json','model-probes.json','navigation.json','live-model-browser.json','exploratory.json','provider-recovery.json','container-recreation.json','final-runtime.json','kernel-prerequisites.json','fresh-environment.json','source-verification.json','core-images.json','browser-images.json','runtime-suite.json','browser-suite.json','T2-repeats.json','recall-delay-published.json','memory-repair-image.json','memory-repair-runtime.json']]
paths+=['private/setup/result.json']+['private/providers/'+x+'.json' for x in ['exercise','after-restart','exploratory','module-down']]
for n in range(1,4):
 paths+=['evidence/T2-repeat-'+str(n)+'/'+x+'.json' for x in ['topology','local-memory','shared-memory','identity']]
for topology in ('T2','T3'):
 paths+=['evidence/memory-repair-'+topology+'/'+x+'.json' for x in ['topology','local-memory','shared-memory','identity','semantic-memory']]
def clean(v):
 if isinstance(v,dict):return {k:clean(vv) for k,vv in v.items() if k not in ('detail','response','stdout','stderr','observations','env','password','token','bootstrapPassword')}
 if isinstance(v,list):return [clean(x) for x in v]
 return v
print(json.dumps({x:clean(json.loads((p/x).read_text())) for x in paths if (p/x).exists()},indent=2,ensure_ascii=False))
