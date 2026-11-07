#!/usr/bin/env python3
"""Generic unavailable-adapter and canonical-source contracts, not memory-owner parity."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[1]
CASES = {
    './memory': ['TestOpaqueDataJSONKeepsVersionedFieldsAndWideIntegerTokens', 'TestOpaqueDataJSONEnvelopeErrorsDoNotReachAuthority', 'TestOpaqueDataJSONPreservesActualAuthorityRefusalWithoutRetry', 'TestStorePreservesRefusals', 'TestBackendSetupSelectionAndEnvironmentPrecedence', 'TestBackendSetupRejectsRetiredSelections'],
    './cmd/aimee-module': ['TestBundledMemoryIsUnavailableBeforeBusAttachment', 'TestModuleRegistryMatchesProcessContracts', 'TestAdvertisedStagesMatchTheContractFile'],
    './db': ['TestMemorySDKDoesNotImportServerDomainStorage', 'TestSharedDatabaseContractHasNoProviderOrDomainDependency', 'TestRuntimeCallersDoNotReintroduceNumberedDatabasePackages'],
    './modules/aimee/families': ['TestDelegateExperienceHostCustodyAndCanonicalSchema', 'TestDelegateExperiencePostgresNextRunReplayAndSourceFences', 'TestExecutionGradeRecallRequiresHost', 'TestExecutionGradeRecallPostgresNextRunScopeAndErasure'],
    './modules/postgres': ['TestSharedDatabaseConcurrentFirstMigration', 'TestSharedDatabaseConcurrentLedgerBootstrap', 'TestSharedDatabaseBothDomainSchemas', 'TestSharedDatabaseRuntimeCannotRewriteMigrationHistory', 'TestSharedDatabaseMigrationFailureAndReplay', 'TestSharedDatabaseStartupRejectsHistoryDrift', 'TestSharedDatabaseSchemaLockCancellation', 'TestSharedDatabaseNativeAndGoBootstrapShareLock', 'TestSharedDatabaseWorkflowAdmissionAcrossProviders', 'TestSharedDatabaseWorkflowSiblingClaimsAcrossProviders', 'TestSharedDatabaseWorkflowStopAndChildCreation', 'TestSharedDatabaseWorkflowBudgetReplyOrder'],
}


def verify(events, required):
    if any(e.get('Action') in ('fail', 'skip') for e in events):
        raise ValueError('failed or skipped required contract')
    passed = {e['Test'] for e in events if e.get('Action') == 'pass' and e.get('Test')}
    if not set(required) <= passed:
        raise ValueError('missing executed required cases: ' + repr(sorted(set(required) - passed)))
    return sorted(passed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if not os.environ.get('AIMEE_TEST_PG_URL') or not os.environ.get('AIMEE_DB_TEST_URL'):
        raise ValueError('adapter/canonical gate requires two explicit disposable PostgreSQL admin DSNs')
    env = dict(os.environ, AIMEE_DB_TEST_REQUIRED='1')
    report = {'qualification': 'generic-adapter-canonical-producers/v1', 'status': 'pass',
              'memory_owner_parity': 'not claimed; selected external implementation requires separate actual qualification',
              'historical_mr18_and_old_current_serving': 'unavailable; frozen manifests unchanged', 'packages': {}, 'source_sha256': {}}
    # Preserve all generic metric/denominator/schema/event helper assertions;
    # this does not execute or recertify the historical owner behavior cases.
    python_code = """
import json,sys,unittest
names=json.load(open(sys.argv[1]))['python_tests']
if len(names)!=40 or len(set(names))!=40: raise ValueError('review generic invariant inventory')
suite=unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromName(n) for n in names)
r=unittest.TextTestRunner(stream=sys.stderr).run(suite)
print(json.dumps({'passed':r.testsRun,'skipped':len(r.skipped)}))
sys.exit(0 if r.wasSuccessful() and not r.skipped and r.testsRun==40 else 1)
"""
    metrics=subprocess.run([sys.executable,'-c',python_code,str(ROOT/'tests/eval/memory_reliability_release_v1.json')],cwd=ROOT,capture_output=True,text=True,timeout=180)
    if metrics.returncode: raise ValueError('generic invariant tests failed: '+metrics.stderr)
    report['generic_python_invariants']=json.loads(metrics.stdout)
    for package, required in CASES.items():
        run = subprocess.run(['go', 'test', '-json', '-race', '-count=1', '-timeout=15m', package, '-run', '^(' + '|'.join(required) + ')$'], cwd=ROOT / 'server-go', env=env, text=True, capture_output=True, timeout=960)
        if run.returncode:
            raise ValueError(package + ': ' + run.stdout[-10000:] + run.stderr[-2000:])
        events = [json.loads(line) for line in run.stdout.splitlines() if line.strip()]
        report['packages'][package] = verify(events, required)
        for path in (ROOT / 'server-go' / package.removeprefix('./')).glob('*.go'):
            report['source_sha256'][str(path.relative_to(ROOT))] = hashlib.sha256(path.read_bytes()).hexdigest()
        print(package, len(required), 'required cases passed', flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(dir=args.output.parent, prefix='.' + args.output.name)
    try:
        with os.fdopen(fd, 'w') as output:
            json.dump(report, output, indent=2)
            output.write('\n')
        os.replace(temporary, args.output)
    finally:
        if os.path.exists(temporary): os.unlink(temporary)


if __name__ == '__main__': main()
