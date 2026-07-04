import { describe, it, expect } from 'vitest';
import { toDashData, fmtDuration, fmtCompact, fmtUsd } from './Dashboard';
import live from './__fixtures__/dashboard-all.json';

/**
 * Regression guard for the dashboard data contract.
 *
 * The server (`dashboard.all`) returns several fields in shapes that do NOT
 * match a panel's row type: `memory_stats` is an OBJECT whose per-kind rows
 * live under `tier_kinds`, `agents` is the provider-config roster (not a live
 * presence feed), and `onboard` is an `{error}` stub on the server-hosted
 * webchat. Earlier the transform silently coerced these to empty, so the
 * Memory and Agents panels rendered blank even though the server had data.
 *
 * These tests feed `toDashData` the REAL captured payload and assert the
 * panels receive populated, correctly-shaped rows.
 */
describe('toDashData — server payload contract', () => {
  const d = toDashData(live as unknown as Parameters<typeof toDashData>[0]);

  it('populates Memory from the memory_stats object (tier_kinds), not the raw array', () => {
    // memory_stats arrives as an object, but memory rows must be non-empty.
    expect(Array.isArray(d.memory)).toBe(true);
    expect(d.memory.length).toBeGreaterThan(0);
    const row = d.memory[0];
    expect(row).toHaveProperty('tier');
    expect(row).toHaveProperty('kind');
    expect(typeof row.count).toBe('number');
    // Total recorded memories should be reflected, not dropped to zero.
    const total = d.memory.reduce((n, r) => n + (r.count || 0), 0);
    expect(total).toBeGreaterThan(0);
  });

  it('populates Agents from the config roster shape (provider/model/roles/enabled)', () => {
    expect(d.agents.length).toBeGreaterThan(0);
    const a = d.agents[0];
    expect(a.name).toBeTruthy();
    expect(a.provider).toBeTruthy();
    expect(a.model).toBeTruthy();
    expect(Array.isArray(a.roles)).toBe(true);
    // The panel keys its status dot on `enabled`; the field must survive.
    expect(typeof a.enabled).toBe('boolean');
  });

  it('keeps the panels that already matched populated', () => {
    expect(d.delegations.length).toBeGreaterThan(0);
    expect(d.metrics.length).toBeGreaterThan(0);
    expect(d.traces.length).toBeGreaterThan(0);
    expect(d.plans.length).toBeGreaterThan(0);
  });

  it('populates the new Cost / Tokens panel from token_audit', () => {
    expect(d.tokenAudit.length).toBeGreaterThan(0);
    const t = d.tokenAudit[0];
    expect(t).toHaveProperty('role');
    expect(typeof t.prompt_tokens).toBe('number');
    expect(typeof t.estimated_cost_usd).toBe('number');
  });

  it('populates the new Governance panel from decisions', () => {
    expect(d.decisions.length).toBeGreaterThan(0);
    const dec = d.decisions[0];
    expect(dec.chosen).toBeTruthy();
    expect(dec).toHaveProperty('outcome');
    expect(dec).toHaveProperty('rationale');
  });

  it('populates the guardrail audit (server-incurred tool-action verdicts)', () => {
    expect(d.audit.length).toBeGreaterThan(0);
    const a = d.audit[0];
    expect(a).toHaveProperty('verdict');
    expect(a).toHaveProperty('tool');
    expect(a).toHaveProperty('actor');
  });

  it('populates the new Active Sessions panel', () => {
    expect(Array.isArray(d.sessions)).toBe(true);
    expect(d.sessions.length).toBeGreaterThan(0);
    expect(d.sessions[0]).toHaveProperty('cwd');
  });

  it('surfaces a real readiness report (steps present, not the {error} stub)', () => {
    expect(d.onboard).not.toBeNull();
    expect(d.onboard?.steps.length).toBeGreaterThan(0);
  });

  it('normalizes the lsp object without throwing', () => {
    expect(d.lsp).not.toBeUndefined();
  });
});

describe('toDashData — defensive coercion', () => {
  it('handles a null/empty payload without throwing', () => {
    const d = toDashData(null);
    expect(d.memory).toEqual([]);
    expect(d.agents).toEqual([]);
    expect(d.delegations).toEqual([]);
    expect(d.onboard).toBeNull();
    expect(d.lsp).toBeNull();
    expect(d.tokenAudit).toEqual([]);
    expect(d.decisions).toEqual([]);
    expect(d.sessions).toEqual([]);
  });

  it('tolerates memory_stats delivered as a legacy flat array', () => {
    const d = toDashData({ memory_stats: [{ tier: 'L0', kind: 'fact', count: 2 }] });
    expect(d.memory).toHaveLength(1);
    expect(d.memory[0].count).toBe(2);
  });

  it('accepts a real onboard report (with steps) instead of nulling it', () => {
    const d = toDashData({
      onboard: { version: '1', ready: true, elapsed_ms: 5, steps: [{ step: 'db', status: 'ok' }], next_actions: [] },
    });
    expect(d.onboard).not.toBeNull();
    expect(d.onboard?.steps).toHaveLength(1);
  });
});

describe('formatters', () => {
  it('fmtDuration renders human-readable durations, not raw ms', () => {
    expect(fmtDuration(482)).toBe('482ms');
    expect(fmtDuration(4662)).toBe('4.7s');
    expect(fmtDuration(42027)).toBe('42s');
    expect(fmtDuration(127428)).toBe('2m 7s'); // the "ridiculous 127248ms" case
    expect(fmtDuration(-1)).toBe('—');
  });

  it('fmtCompact abbreviates large token counts', () => {
    expect(fmtCompact(240)).toBe('240');
    expect(fmtCompact(6250)).toBe('6.3k');
    expect(fmtCompact(4380809)).toBe('4.4M');
  });

  it('fmtUsd formats cost with a sub-cent floor', () => {
    expect(fmtUsd(0)).toBe('$0.00');
    expect(fmtUsd(0.001)).toBe('<$0.01');
    expect(fmtUsd(4.21)).toBe('$4.21');
  });
});
