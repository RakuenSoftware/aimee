import { describe, expect, it } from 'vitest';
import { completedSteps, computeReadiness, readinessKeysAreDocumented, stepsRemaining } from './readiness';

const readySignals = { accountReady: true, projectCount: 1, gitIdentityReady: true };
describe('single-user readiness', () => {
  it('needs no KB or shared database', () => {
    const r = computeReadiness({ provider: 'claude', embedder_model: 'bekko-a25m' }, readySignals);
    expect(r.ready).toBe(true);
    expect(stepsRemaining(r)).toBe(0);
    expect(r.steps).not.toHaveProperty('knowledge_base');
    expect(r.steps).not.toHaveProperty('db2');
  });
  it('connecting to a KB never supplies local embedding', () => {
    const cfg = { provider: 'claude', kb_mode: 'remote', kb_client_url: 'https://kb.example' };
    const r = computeReadiness(cfg, readySignals);
    expect(r.ready).toBe(false);
    expect(r.steps.embedding.ok).toBe(false);
    expect(stepsRemaining(r)).toBe(1);
    expect(computeReadiness({ ...cfg, embedder_model: 'bekko-a25m' }, readySignals).ready).toBe(true);
  });
  it('an incomplete optional KB does not block personal setup', () => {
    expect(computeReadiness({ provider: 'claude', kb_mode: 'remote', embedder_model: 'bekko-a25m' }, readySignals).ready).toBe(true);
  });
  it('still requires account, provider, models, commit identity and a project', () => {
    const r = computeReadiness({}, { accountReady: false, projectCount: 0 });
    expect(stepsRemaining(r)).toBe(5);
    expect(r.steps.connection.optional).toBe(true);
    expect(completedSteps({}, { accountReady: false, projectCount: 0 }).size).toBe(0);
  });
  it('accepts a configured model, command, or external embedder', () => {
    for (const cfg of [{ embedder_model: 'bekko-a25m' }, { embedder_command: 'embed.sh' }, { embedder_url: 'https://embed.example' }]) {
      expect(computeReadiness(cfg, readySignals).steps.embedding.ok).toBe(true);
    }
  });
  it('keeps optional git connections and completed steps accurate', () => {
    const cfg = { provider: 'claude', embedder_model: 'bekko-a25m' };
    expect(completedSteps(cfg, { ...readySignals, hostsConnected: 2 }).size).toBe(6);
    expect(computeReadiness(cfg, { ...readySignals, hostsConnected: 2 }).steps.connection.detail).toBe('2 hosts connected');
    expect(computeReadiness(cfg, { ...readySignals, projectCount: 0 }).ready).toBe(false);
    expect(readinessKeysAreDocumented()).toBe(true);
  });
});
