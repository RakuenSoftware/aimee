#!/usr/bin/env python3
"""Enforce explicit public generic versus legacy-owner compatibility test boundaries."""
from pathlib import Path
import hashlib
import json
import re
ROOT=Path(__file__).resolve().parents[1]

def validate(root):
    directory=root/'server-go/modules/memory'
    files=sorted(str(p.relative_to(directory)) for p in directory.rglob('*.go'))
    if files!=['adapter/unavailable.go']:
        raise ValueError('retired concrete owner restored or generic adapter missing')
    metadata=json.loads((root/'tests/baselines/modules/memory-compatibility-extraction.json').read_text())
    cases=metadata['cases']
    if len(cases)!=6 or len({x['source'] for x in cases})!=6:
        raise ValueError('review exact historical compatibility inventory')
    retired=metadata.get('retired_cases',[])
    if len(retired)!=1 or retired[0]['target']!='unit-test-benchmark-context-transport' or retired[0]['sha256']!='caeac77dc7bfe3cf1c0b68db6235054e74bf3f18c9419525a7e7ff2aa33333d8':
        raise ValueError('original retired benchmark fixture finding changed')
    if (root/retired[0]['source']).exists() or retired[0].get('retirement',{}).get('repository')!='RakuenSoftware/aims-db' or retired[0]['retirement'].get('source')!='experiments/legacy-aimee-go/public-owner-retirement-fixtures/20261025/retired-public-contracts/src/tests/test_benchmark_context_transport.c':
        raise ValueError('retired benchmark fixture restored or historical reference missing')
    actual={str(p.relative_to(root)) for p in (root/'src/tests').glob('*.c') if re.search(r'\bmodule_runtime_fixture_call\s*\(', p.read_text())}
    if actual!={x['source'] for x in cases}:
        raise ValueError('unclassified legacy fixture consumer')
    for case in cases:
        if hashlib.sha256((root/case['source']).read_bytes()).hexdigest()!=case['sha256']:
            raise ValueError('legacy assertion source changed without inventory review')
    rules=(root/'src/tests/Rules.mk').read_text()
    match=re.search(r'LEGACY_MEMORY_COMPAT_TARGETS := (.*?)\nTEST_TARGETS := \$\(filter-out \$\(LEGACY_MEMORY_COMPAT_TARGETS\),\$\(TEST_TARGETS\)\)',rules,re.S)
    if not match or set(re.findall(r'unit-test-[a-z0-9-]+',match[1]))!={x['target'] for x in cases}:
        raise ValueError('generic suite no longer explicitly separates compatibility cases')
    normal=re.search(r'^unit-tests: (.*?)(?=\n\t)',rules,re.M|re.S)
    if not normal or 'aimee-memory-fixture' in normal[1]:
        raise ValueError('default generic suite invokes a legacy owner fixture')
    if './modules/memory/testdata/nativefixture' in rules:
        raise ValueError('public build still invokes deleted concrete fixture')
    adapter=(directory/'adapter/unavailable.go').read_text()
    if 'memory capability unavailable' not in adapter or 'func Unavailable() error' not in adapter:
        raise ValueError('bundled adapter falsely claims implementation')

if __name__=='__main__':
    validate(ROOT)
    print('memory-retirement-boundary: unavailable adapter; six explicitly opt-in legacy compatibility binaries; generic contracts retained')
