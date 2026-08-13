import { describe, it, expect } from 'vitest';
import { WIZARD_STEPS, visibleSteps, isRestartKey, helpFor } from './wizardSteps';
import { RESTART_KEYS, FIELD_HELP } from '../pages/settingsHelp';
import { saveConfigValue, loadConfig } from './configApi';
import type { StepId } from './readiness';

describe('WIZARD_STEPS structure', () => {
  it('covers every readiness StepId exactly once, in dependency order', () => {
    const ids = WIZARD_STEPS.map((s) => s.id);
    expect(ids).toEqual<StepId[]>(['account', 'provider', 'knowledge_base', 'embedding', 'db2', 'git_identity', 'connection', 'project']);
    expect(new Set(ids).size).toBe(ids.length); // no dupes
  });

  it('account is first, provider is the chooser, and the KB fork follows', () => {
    expect(WIZARD_STEPS[0].kind).toBe('account');
    const provider = WIZARD_STEPS.find((s) => s.id === 'provider')!;
    expect(provider.kind).toBe('chooser');
    const kb = WIZARD_STEPS[2];
    expect(kb.id).toBe('knowledge_base');
    expect(kb.kind).toBe('kb');
    expect(kb.keys).toEqual([]);
  });

  it('deploy-topology + DB2 are local-only; the tail is identity → connection → workspaces', () => {
    const embedding = WIZARD_STEPS.find((s) => s.id === 'embedding')!;
    const db2 = WIZARD_STEPS.find((s) => s.id === 'db2')!;
    expect(embedding.kind).toBe('deploy');
    expect(embedding.showWhen!('local')).toBe(true);
    expect(embedding.showWhen!('remote')).toBe(false);
    // DB2 is a bespoke step (bundled vs existing Postgres); db2_url is no longer a
    // required generic key.
    expect(db2.kind).toBe('db2');
    expect(db2.keys).toEqual([]);
    expect(db2.showWhen!('local')).toBe(true);
    expect(db2.showWhen!('remote')).toBe(false);

    const connection = WIZARD_STEPS.find((s) => s.id === 'connection')!;
    const identity = WIZARD_STEPS.find((s) => s.id === 'git_identity')!;
    const project = WIZARD_STEPS.find((s) => s.id === 'project')!;
    expect(connection.optional).toBe(true);
    expect(identity.kind).toBe('git_identity');
    expect(identity.optional).not.toBe(true);
    expect(connection.kind).toBe('connection');
    expect(project.kind).toBe('workspace');
    // Folded in: no longer a route hand-off.
    expect(project.keys).toEqual([]);
    expect('route' in project).toBe(false);
  });

  it('visibleSteps forks on kb_mode: remote hides deploy + db2', () => {
    const local = visibleSteps('local').map((s) => s.id);
    const remote = visibleSteps('remote').map((s) => s.id);
    expect(local).toEqual(['account', 'provider', 'knowledge_base', 'embedding', 'db2', 'git_identity', 'connection', 'project']);
    expect(remote).toEqual(['account', 'provider', 'knowledge_base', 'git_identity', 'connection', 'project']);
  });

  it('appliance mode hides the baked-infra steps (kb/deploy/db2)', () => {
    const applianceLocal = visibleSteps('local', true).map((s) => s.id);
    expect(applianceLocal).toEqual(['account', 'provider', 'git_identity', 'connection', 'project']);
    // Same regardless of kb mode — the appliance bakes it.
    expect(visibleSteps('remote', true).map((s) => s.id)).toEqual(['account', 'provider', 'git_identity', 'connection', 'project']);
  });

  it('every keyed step references documented config keys', () => {
    for (const step of WIZARD_STEPS) {
      for (const k of step.keys) {
        expect(FIELD_HELP, `${step.id}: ${k} undocumented`).toHaveProperty(k);
      }
    }
  });
});

describe('isRestartKey / helpFor', () => {
  it('isRestartKey matches exactly the RESTART_KEYS set', () => {
    for (const k of RESTART_KEYS) expect(isRestartKey(k)).toBe(true);
    expect(isRestartKey('provider')).toBe(false);
    // db2_url is a known restart key and appears in the wizard.
    expect(isRestartKey('db2_url')).toBe(true);
  });

  it('helpFor returns the settingsHelp copy, or "" for unknowns', () => {
    expect(helpFor('provider').length).toBeGreaterThan(0);
    expect(helpFor('totally_made_up_key')).toBe('');
  });
});

// Stub the fetch the config API depends on — vitest runs in node, so we control
// the whole request/response.
function stubFetch(status: number, body: unknown): typeof fetch {
  return (async () => ({
    status,
    json: async () => body,
  })) as unknown as typeof fetch;
}

describe('saveConfigValue (wizard write path)', () => {
  it('a 2xx with no error is a success and echoes the server value', async () => {
    const res = await saveConfigValue('provider', 'claude', { fetchImpl: stubFetch(200, { value: 'claude' }) });
    expect(res.ok).toBe(true);
    expect(res.value).toBe('claude');
  });

  it('a 4xx surfaces the server error and does not succeed', async () => {
    const res = await saveConfigValue('db2_url', 'bad', { fetchImpl: stubFetch(400, { error: 'invalid url' }) });
    expect(res.ok).toBe(false);
    expect(res.error).toBe('invalid url');
  });

  it('a 2xx body carrying an error field is still a failure', async () => {
    const res = await saveConfigValue('provider', 'x', { fetchImpl: stubFetch(200, { error: 'nope' }) });
    expect(res.ok).toBe(false);
    expect(res.error).toBe('nope');
  });

  it('a thrown fetch (network error) is caught, not propagated', async () => {
    const throwing = (async () => { throw new Error('network down'); }) as unknown as typeof fetch;
    const res = await saveConfigValue('provider', 'x', { fetchImpl: throwing });
    expect(res.ok).toBe(false);
    expect(res.error).toMatch(/network down/);
  });
});

describe('loadConfig', () => {
  it('unwraps { config } and returns {} on failure', async () => {
    const ok = await loadConfig({ fetchImpl: stubFetch(200, { config: { provider: 'claude' } }) });
    expect(ok).toEqual({ provider: 'claude' });
    const throwing = (async () => { throw new Error('x'); }) as unknown as typeof fetch;
    expect(await loadConfig({ fetchImpl: throwing })).toEqual({});
  });
});
